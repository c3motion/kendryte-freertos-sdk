/* Copyright 2018 Canaan Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdlib.h>
#include <string.h>
#include <FreeRTOS.h>
#include <dmac.h>
#include <hal.h>
#include <kernel/driver_impl.hpp>
#include <kpu.h>
#include <sysctl.h>
#include <math.h>
#include <float.h>
#include <time.h>
#include <sys/time.h>
#include <assert.h>

using namespace sys;

#define KPU_DEBUG 1
#define NNCASE_DEBUG 0
#define USE_CACHED_AI_RAM 0

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))

#define COMMON_ENTRY \
    semaphore_lock locker(free_mutex_);

class k_model_context : public heap_object, public free_object_access
{
public:
    k_model_context(uint8_t *buffer)
    {
        uintptr_t base_addr = (uintptr_t)buffer;
        const kpu_model_header_t *header = (const kpu_model_header_t *)buffer;
        if (header->version == 3 && header->arch == 0)
        {
            model_buffer_ = buffer;
            output_count_ = header->output_count;
            outputs_ = (const kpu_model_output_t *)(base_addr + sizeof(kpu_model_header_t));
            layer_headers_ = (const kpu_model_layer_header_t *)((uintptr_t)outputs_ + sizeof(kpu_model_output_t) * output_count_);
            layers_length_ = header->layers_length;
            body_start_ = (const uint8_t *)((uintptr_t)layer_headers_ + sizeof(kpu_model_layer_header_t) * header->layers_length);
            storage_ = std::make_unique<uint8_t[]>(header->main_mem_usage);
            main_buffer_ = { storage_.get(), ptrdiff_t(header->main_mem_usage) };
        }
        else
        {
            throw std::runtime_error("Cannot load kmodel.");
        }
    }

    void get(kpu_model_context_t *ctx)
    {
        ctx->body_start = body_start_;
        ctx->model_buffer = model_buffer_;
        ctx->main_buffer = main_buffer_.data();
        ctx->layer_headers = layer_headers_;
        ctx->layers_length = layers_length_;
        ctx->output_count = output_count_;
        ctx->outputs = outputs_;
    }
private:
    const uint8_t *model_buffer_;
    const kpu_model_layer_header_t *layer_headers_;
    const uint8_t *body_start_;
    uint32_t layers_length_;
    uint32_t output_count_;
    const kpu_model_output_t * outputs_;
    gsl::span<uint8_t> main_buffer_;
    std::unique_ptr<uint8_t[]> storage_;
};

class k_kpu_driver : public kpu_driver, public static_object, public free_object_access
{
public:
    k_kpu_driver(uintptr_t base_addr, sysctl_clock_t clock, sysctl_dma_select_t dma_req)
        : kpu_(*reinterpret_cast<volatile kpu_config_t *>(base_addr)), clock_(clock), dma_req_(dma_req)
    {
        completion_event_ = xSemaphoreCreateBinary();
    }

    virtual void install() override
    {
        free_mutex_ = xSemaphoreCreateMutex();
        sysctl_clock_disable(clock_);
    }

    virtual void on_first_open() override
    {
        sysctl_clock_enable(clock_);
    }

    virtual void on_last_close() override
    {
        sysctl_clock_disable(clock_);
    }

    virtual handle_t model_load_from_buffer(uint8_t *buffer) override
    {
        return system_alloc_handle(make_accessor(make_object<k_model_context>(buffer)));
    }

    virtual int run(handle_t context, const uint8_t *src) override
    {
        COMMON_ENTRY;

        auto model_context = system_handle_to_object(context).as<k_model_context>();
        model_context->get(&ctx_);
        dma_ch_ = dma_open_free();
        ctx_.current_layer = 0;
        ctx_.current_body = ctx_.body_start;
        
        kpu_model_header_t *header = (kpu_model_header_t *)ctx_.model_buffer;
        kpu_.interrupt_clear.reg = 7;

        kpu_.fifo_threshold.data.fifo_full_threshold = 10;
        kpu_.fifo_threshold.data.fifo_empty_threshold = 1;
        kpu_.fifo_threshold.data.reserved = 0;

        kpu_.eight_bit_mode.data.eight_bit_mode = header->flags & 1;
        kpu_.eight_bit_mode.data.reserved = 0;

        kpu_.interrupt_mask.data.calc_done_int = 1;
        kpu_.interrupt_mask.data.layer_cfg_almost_empty_int = 0;
        kpu_.interrupt_mask.data.layer_cfg_almost_full_int = 1;
        kpu_.interrupt_mask.data.reserved = 0;

        pic_set_irq_priority(IRQN_AI_INTERRUPT, 1);
        pic_set_irq_handler(IRQN_AI_INTERRUPT, kpu_isr_handle, this);
        pic_set_irq_enable(IRQN_AI_INTERRUPT, 1);

        const kpu_model_layer_header_t *first_layer_header = ctx_.layer_headers;
        if (first_layer_header->type != KL_K210_CONV)
            return -1;
        const kpu_model_conv_layer_argument_t *first_layer = (const kpu_model_conv_layer_argument_t *)ctx_.body_start;
        kpu_layer_argument_t layer_arg = *(kpu_layer_argument_t *)(ctx_.model_buffer + first_layer->layer_offset);

#if KPU_DEBUG
        gettimeofday(&last_time_, NULL);
#endif
        if ((layer_arg.image_size.data.i_row_wid + 1) % 64 != 0)
        {
            kpu_input_with_padding(&layer_arg, src);
            ai_step_not_isr();
        }
        else
        {
            kpu_input_dma(&layer_arg, src);
        }
        while (!done_flag_)
        {
            if(xSemaphoreTake(completion_event_, portMAX_DELAY) == pdTRUE)
            {
                if (ctx_.current_layer != ctx_.layers_length)
                {
                    while(ai_step() == 1)
                        ;
                }
                else
                {
                    kpu_done();
                }
            }
        }
        done_flag_ = 0;
        return 0;
    }

    virtual int get_output(handle_t context, uint32_t index, uint8_t **data, size_t *size) override
    {
        COMMON_ENTRY;
        auto model_context = system_handle_to_object(context).as<k_model_context>();
        model_context->get(&ctx_);
        if (index >= ctx_.output_count)
            return -1;

        const kpu_model_output_t *output = ctx_.outputs + index;
        *data = ctx_.main_buffer + output->address;
        *size = output->size;
        return 0;
    }

private:
    static void kpu_isr_handle(void *userdata)
    {
        auto &driver = *reinterpret_cast<k_kpu_driver *>(userdata);

        driver.kpu_.interrupt_clear.data.calc_done_int = 1;
        driver.kpu_.interrupt_clear.data.layer_cfg_almost_empty_int = 1;
        driver.kpu_.interrupt_clear.data.layer_cfg_almost_full_int = 1;
        driver.kpu_.interrupt_clear.data.reserved = 0;

        driver.kpu_.interrupt_mask.data.calc_done_int = 1;
        driver.kpu_.interrupt_mask.data.layer_cfg_almost_empty_int = 1;
        driver.kpu_.interrupt_mask.data.layer_cfg_almost_full_int = 1;
        driver.kpu_.interrupt_mask.data.reserved = 0;

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(driver.completion_event_, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken)
        {
            portYIELD_FROM_ISR();
        }
    }

    void kpu_flush_cache(uint32_t addr, size_t lines)
    {
        size_t line;
        for (line = 0; line < lines; line++)
        {
            const uint64_t *src = (const uint64_t *)(AI_RAM_BASE_ADDR + (addr + line) * 64);
            uint64_t *dest = (uint64_t *)(AI_IO_BASE_ADDR + (addr + line) * 64);
            size_t i;
            for (i = 0; i < 8; i++)
                dest[i] = src[i];
        }
    }

    void kpu_send_layer(const kpu_layer_argument_t *layer)
    {
        kpu_.layer_argument_fifo = layer->interrupt_enabe.reg;
        kpu_.layer_argument_fifo = layer->image_addr.reg;
        kpu_.layer_argument_fifo = layer->image_channel_num.reg;
        kpu_.layer_argument_fifo = layer->image_size.reg;
        kpu_.layer_argument_fifo = layer->kernel_pool_type_cfg.reg;
        kpu_.layer_argument_fifo = layer->kernel_load_cfg.reg;
        kpu_.layer_argument_fifo = layer->kernel_offset.reg;
        kpu_.layer_argument_fifo = layer->kernel_calc_type_cfg.reg;
        kpu_.layer_argument_fifo = layer->write_back_cfg.reg;
        kpu_.layer_argument_fifo = layer->conv_value.reg;
        kpu_.layer_argument_fifo = layer->conv_value2.reg;
        kpu_.layer_argument_fifo = layer->dma_parameter.reg;
    }

    void kpu_upload_core(size_t width, size_t height, size_t channels, const uint8_t *src, uint32_t kpu_addr)
    {
        uint8_t *dest = (uint8_t *)AI_IO_BASE_ADDR + kpu_addr * 64;
        size_t oc, y, x;
        uint32_t row_padding;
        uint32_t row_group;
        uint32_t row_length;
        if (width <= 16)
        {
        	row_padding = 16;
        	row_group = 4;
        	row_length = 1;
        }
        else if (width <= 32)
        {
        	row_padding = 32;
        	row_group = 2;
        	row_length = 1;
        }
        else
        {
        	row_padding = 64;
        	row_group = 1;
        	row_length = (width + 63) / 64;
        }

        if ((uintptr_t)src % 8 == 0 && width % 8 == 0)
        {
            width /= 8;
            const uint64_t *u64_src = (const uint64_t *)src;
            for (oc = 0; oc < channels; oc++)
            {
            	uint8_t *channel_origin = dest + oc / row_group * row_length * height * 64 + oc % row_group * row_padding;
            	for (y = 0; y < height; y++)
            	{
            		uint64_t *y_origin = (uint64_t *)channel_origin + y * row_length * 64L;
            		for (x = 0; x < width; x++)
                    {
                    y_origin[x] = *u64_src++;
#if NNCASE_DEBUG
                    assert(y_origin > AI_IO_BASE_ADDR && y_origin < (AI_IO_BASE_ADDR + 2 * 1024 * 1024));
#endif
                }
                }
            }
        }
        else
        {
            for (oc = 0; oc < channels; oc++)
            {
            	uint8_t *channel_origin = dest + oc / row_group * row_length * height * 64 + oc % row_group * row_padding;
            	for (y = 0; y < height; y++)
            	{
            		uint8_t *y_origin = channel_origin + y * row_length * 64;
            		for (x = 0; x < width; x++)
                        y_origin[x] = *src++;
                }
            }
        }
    }

    void kpu_input_dma(const kpu_layer_argument_t *layer, const uint8_t *src)
    {
        uint64_t input_size = layer->kernel_calc_type_cfg.data.channel_switch_addr * 64 * (layer->image_channel_num.data.i_ch_num + 1);

        dma_set_request_source(dma_ch_, dma_req_);
        dma_transmit_async(dma_ch_, src, (void *)(uintptr_t)((uint8_t *)AI_IO_BASE_ADDR + layer->image_addr.data.image_src_addr * 64), 1, 1, sizeof(uint64_t), input_size / 8, 16, completion_event_);
    }

    void kpu_input_with_padding(const kpu_layer_argument_t *layer, const uint8_t *src)
    {
        size_t width = layer->image_size.data.i_row_wid + 1;
        size_t height = layer->image_size.data.i_col_high + 1;
        size_t channels = layer->image_channel_num.data.i_ch_num + 1;
        kpu_upload_core(width, height, channels, src, layer->image_addr.data.image_src_addr);
    }

    void kpu_fully_connected(const float *src, const float *weights, const float *biases, float *dest, int input_channels, int output_channels)
    {
        int ic, oc;
        for (oc = 0; oc < output_channels; oc++)
        {
            const float *c_weights = weights + oc * input_channels;
    
            float sum = 0.0f;
            for (ic = 0; ic < input_channels; ic++)
                sum += src[ic] * c_weights[ic];
            dest[oc] = sum + biases[oc];
        }
    }

    void kpu_add(const kpu_model_add_layer_argument_t *arg)
    {
        const float *src_a = (const float *)(ctx_.main_buffer + arg->main_mem_in_a_address);
        const float *src_b = (const float *)(ctx_.main_buffer + arg->main_mem_in_b_address);
        float *dest = (float *)(ctx_.main_buffer + arg->main_mem_out_address);
        size_t i, count = arg->count;
        
        for (i = 0; i < count; i++)
            dest[i] = src_a[i] + src_b[i];
    }

    void kpu_quantized_add(const kpu_model_quant_add_layer_argument_t *arg)
    {
        const uint8_t *src_a = (const uint8_t *)(ctx_.main_buffer + arg->main_mem_in_a_address);
        const uint8_t *src_b = (const uint8_t *)(ctx_.main_buffer + arg->main_mem_in_b_address);
        size_t count = arg->count;
        int64_t off_a = arg->in_a_offset, mul_a = arg->in_a_mul, sh_a = arg->in_a_shift;
        int64_t off_b = arg->in_b_offset, mul_b = arg->in_b_mul, sh_b = arg->in_b_shift;
        int64_t off_o = arg->out_offset, mul_o = arg->out_mul, sh_o = arg->out_shift;
    
        uint8_t *dest = (uint8_t *)(ctx_.main_buffer + arg->main_mem_out_address);
        size_t i;

        if (sh_a == sh_b)
        {
            for (i = 0; i < count; i++)
            {
                int64_t a = (*src_a++ + off_a) * mul_a;
                int64_t b = (*src_b++ + off_b) * mul_b;
                int64_t value = (((a + b) >> sh_a) * mul_o >> sh_o) + off_o;
                if (value < 0) value = 0;
                if (value > 0xFF) value = 0xFF;
                *dest++ = (uint8_t)value;
            }
        }
        else
        {
            for (i = 0; i < count; i++)
            {
                int64_t a = (*src_a++ + off_a) * mul_a >> sh_a;
                int64_t b = (*src_b++ + off_b) * mul_b >> sh_b;
                int64_t value = ((a + b) * mul_o >> sh_o) + off_o;
                if (value < 0) value = 0;
                if (value > 0xFF) value = 0xFF;
                *dest++ = (uint8_t)value;
            }
        }
    }

    void kpu_global_average_pool2d(const kpu_model_gap2d_layer_argument_t *arg)
    {
        const float *src = (const float *)(ctx_.main_buffer + arg->main_mem_in_address);
        float *dest = (float *)(ctx_.main_buffer + arg->main_mem_out_address);
        size_t oc, channels = arg->channels, kernel_size = arg->kernel_size;
        
        for (oc = 0; oc < channels; oc++)
        {
            float sum = 0.f;
            size_t i;
            for (i = 0; i < kernel_size; i++)
                sum += *src++;

            dest[oc] = sum / kernel_size;
        }
    }

    void kpu_quantized_max_pool2d(const kpu_model_quant_max_pool2d_layer_argument_t *arg)
    {
        const uint8_t *src = (const uint8_t *)(ctx_.main_buffer + arg->main_mem_in_address);
        uint8_t *dest = (uint8_t *)(ctx_.main_buffer + arg->main_mem_out_address);
        kpu_model_shape_t in_shape = arg->in_shape, out_shape = arg->out_shape;
        uint32_t kernel_width = arg->kernel_width, kernel_height = arg->kernel_height;
        uint32_t stride_width = arg->stride_width, stride_height = arg->stride_height;
        uint32_t padding_width = arg->padding_width, padding_height = arg->padding_height;
    
        uint32_t out_y, out_x, oc;
    
        for (oc = 0; oc < out_shape.channels; oc++)
        {
            const uint8_t *channel_src = src + in_shape.width * in_shape.height * oc;
            for (out_y = 0; out_y < out_shape.height; out_y++)
            {
                for (out_x = 0; out_x < out_shape.width; out_x++)
                {
                    int32_t in_x_origin = (int32_t)(out_x * stride_width) - padding_width;
                    int32_t in_y_origin = (int32_t)(out_y * stride_height) - padding_height;
                    int32_t kernel_x_start = max(0, -in_x_origin);
                    int32_t kernel_x_end = min(kernel_width, in_shape.width - in_x_origin);
                    int32_t kernel_y_start = max(0, -in_y_origin);
                    int32_t kernel_y_end = min(kernel_height, in_shape.height - in_y_origin);
                    uint8_t value = 0;
    
                    int32_t kernel_y, kernel_x;
                    for (kernel_y = kernel_y_start; kernel_y < kernel_y_end; kernel_y++)
                    {
                        for (kernel_x = kernel_x_start; kernel_x < kernel_x_end; kernel_x++)
                        {
                            int32_t in_x = in_x_origin + kernel_x;
                            int32_t in_y = in_y_origin + kernel_y;
                            value = max(value, channel_src[in_y * in_shape.width + in_x]);
                        }
                    }
    
                    *dest++ = value;
                }
            }
        }
    }

    void kpu_quantize(const kpu_model_quantize_layer_argument_t *arg)
    {
        size_t count = arg->count;
        const float *src = (const float *)(ctx_.main_buffer + arg->main_mem_in_address);;
        const kpu_model_quant_param_t q = arg->quant_param;
        float scale = 1.f / q.scale;

        uint8_t *dest = (uint8_t *)(ctx_.main_buffer + arg->mem_out_address);
        size_t i;
        for (i = 0; i < count; i++)
        {
            int value = (*src++ - q.bias) * scale;
            if (value < 0) value = 0;
            if (value > 0xFF) value = 0xFF;
            *dest++ = (uint8_t)value;
        }
    }

    void kpu_dequantize(const kpu_model_dequantize_layer_argument_t *arg)
    {
        const uint8_t *src = (const uint8_t *)(ctx_.main_buffer + arg->main_mem_in_address);
        float *dest = (float *)(ctx_.main_buffer + arg->main_mem_out_address);
        size_t oc, count = arg->count;
        const kpu_model_quant_param_t q = arg->quant_param;
        
        for (oc = 0; oc < count; oc++)
            dest[oc] = *src++ * q.scale + q.bias;
    }

    void kpu_requantize(const kpu_model_requantize_layer_argument_t *arg)
    {
        const uint8_t *src = (const uint8_t *)(ctx_.main_buffer + arg->main_mem_in_address);
        uint8_t *dest = (uint8_t *)(ctx_.main_buffer + arg->main_mem_out_address);
        size_t oc, count = arg->count;
        const uint8_t *table = arg->table;
        
        for (oc = 0; oc < count; oc++)
            dest[oc] = table[*src++];
    }

    void kpu_l2_normalization(const kpu_model_l2_norm_layer_argument_t *arg)
    {
        const float *src = (const float *)(ctx_.main_buffer + arg->main_mem_in_address);
        float *dest = (float *)(ctx_.main_buffer + arg->main_mem_out_address);
        size_t oc, channels = arg->channels;

        float sum = 0.f;
        const float epsilon = 1e-10f;
        for (oc = 0; oc < channels; oc++)
            sum += src[oc] * src[oc];
        if (sum < epsilon)
            sum = epsilon;
        sum = 1.f / sqrtf(sum);
        for (oc = 0; oc < channels; oc++)
            dest[oc] = src[oc] * sum;
    }

    void kpu_softmax(const kpu_model_softmax_layer_argument_t *arg)
    {
        const float *src = (const float *)(ctx_.main_buffer + arg->main_mem_in_address);
        float *dest = (float *)(ctx_.main_buffer + arg->main_mem_out_address);
        size_t oc, channels = arg->channels;
    
        float max = FLT_MIN;
        for (oc = 0; oc < channels; oc++)
            max = fmaxf(max, src[oc]);
    
        float sum = 0.f;
        for (oc = 0; oc < channels; oc++)
        {
            float value = expf(src[oc] - max);
            sum += value;
            dest[oc] = value;
        }
            
        for (oc = 0; oc < channels; oc++)
            dest[oc] /= sum;
    }

    void kpu_concat(const kpu_model_concat_layer_argument_t *arg)
    {
        uint8_t *dest = (uint8_t *)(ctx_.main_buffer + arg->main_mem_out_address);
        uint32_t count = arg->input_count, i;
    
        for (i = 0; i < count; i++)
        {
            kpu_model_memory_range_t input = arg->inputs_mem[i];
            const uint8_t *src = (const uint8_t *)(ctx_.main_buffer + input.start);
            memcpy(dest, src, input.size);
            dest += input.size;
        }
    }

    void kpu_conv(const kpu_model_conv_layer_argument_t *arg)
    {
        volatile kpu_layer_argument_t layer = *(kpu_layer_argument_t *)(ctx_.model_buffer + arg->layer_offset);
        layer.kernel_load_cfg.data.para_start_addr = (uintptr_t)(ctx_.model_buffer + arg->weights_offset);
        layer.kernel_pool_type_cfg.data.bwsx_base_addr = (uintptr_t)(ctx_.model_buffer + arg->bn_offset);
        layer.kernel_calc_type_cfg.data.active_addr = (uintptr_t)(ctx_.model_buffer + arg->act_offset);

        if (arg->flags & KLF_MAIN_MEM_OUT)
        {
            uint8_t *dest = ctx_.main_buffer + arg->main_mem_out_address;

            layer.dma_parameter.data.send_data_out = 1;

            dma_set_request_source(dma_ch_, dma_req_);
            dma_transmit_async(dma_ch_, (void *)(&kpu_.fifo_data_out), dest, 0, 1, sizeof(uint64_t), (layer.dma_parameter.data.dma_total_byte + 8) / 8, 8, completion_event_);
        }
        else
        {
#if KPU_DEBUG
            kpu_.interrupt_mask.data.calc_done_int = 0;
            kpu_.interrupt_mask.data.layer_cfg_almost_empty_int = 1;
            kpu_.interrupt_mask.data.layer_cfg_almost_full_int = 1;
            kpu_.interrupt_mask.data.reserved = 0;
            layer.interrupt_enabe.data.int_en = 1;
#else
            kpu_.interrupt_mask.data.calc_done_int = 1;
            kpu_.interrupt_mask.data.layer_cfg_almost_empty_int = 0;
            kpu_.interrupt_mask.data.layer_cfg_almost_full_int = 1;
            kpu_.interrupt_mask.data.reserved = 0;
#endif
        }
        kpu_send_layer((const kpu_layer_argument_t *)&layer);
    }

    void kpu_add_padding(const kpu_model_add_padding_layer_argument_t *arg)
    {
        const uint8_t *src = (const uint8_t *)(ctx_.main_buffer + arg->main_mem_in_address);

#if USE_CACHED_AI_RAM
        uint8_t *dest = (uint8_t *)AI_RAM_BASE_ADDR + arg->kpu_mem_out_address * 64;
#else
        uint8_t *dest = (uint8_t *)AI_IO_BASE_ADDR + arg->kpu_mem_out_address * 64;
#endif

        uint32_t row_padding = 16;
        uint32_t row_group = 4;
        uint32_t row_length = 1;
        uint32_t height = 4;
        uint32_t oc, x, y, channels = arg->channels;

        for (oc = 0; oc < channels; oc++)
        {
            uint8_t *channel_origin = dest + oc / row_group * row_length * height * 64 + oc % row_group * row_padding;
            for (y = 0; y < 1; y++)
            {
                uint8_t *y_origin = channel_origin + y * row_length * 64;
                for (x = 0; x < 1; x++)
                    y_origin[x] = *src++;
            }
        }
#if USE_CACHED_AI_RAM
        uint32_t lines = row_length * height * channels / row_group;
        kpu_flush_cache(arg->kpu_mem_out_address, lines);
#endif
    }

    void kpu_remove_padding(const kpu_model_remove_padding_layer_argument_t *arg)
    {
        const uint8_t *src = (const uint8_t *)(ctx_.main_buffer + arg->main_mem_in_address);
        uint8_t *dest = (uint8_t *)(ctx_.main_buffer + arg->main_mem_out_address);
        uint32_t oc, channels = arg->channels;

        for (oc = 0; oc < channels; oc++)
            *dest++ = src[oc * 16];
    }

    void kpu_upload(const kpu_model_upload_layer_argument_t *arg)
    {
        size_t width = arg->width;
        size_t height = arg->height;
        size_t channels = arg->channels;

        kpu_upload_core(width, height, channels, ctx_.main_buffer + arg->main_mem_in_address, arg->kpu_mem_out_address);
    }

#if KPU_DEBUG
    const char *str_layer_type(uint32_t type)
    {
        switch (type)
        {
            case KL_ADD:
                return "Add";
            case KL_QUANTIZED_ADD:
                return "QuantAdd";
            case KL_GLOBAL_AVERAGE_POOL2D:
                return "GAP";
            case KL_QUANTIZED_MAX_POOL2D:
                return "QuantMaxPool2d";
            case KL_QUANTIZE:
                return "Quantize";
            case KL_DEQUANTIZE:
                return "Dequantize";
            case KL_REQUANTIZE:
                return "Requantize";
            case KL_L2_NORMALIZATION:
                return "L2Norm";
            case KL_SOFTMAX:
                return "Softmax";
            case KL_CONCAT:
                return "Concat";
            case KL_QUANTIZED_CONCAT:
                return "QuantConcat";
            case KL_K210_CONV:
                return "K210Conv";
            case KL_K210_ADD_PADDING:
                return "K210AddPad";
            case KL_K210_REMOVE_PADDING:
                return "K210RemovePad";
            case KL_K210_UPLOAD:
                return "K210Upload";
            default:
                return "Unknown";
        }
    }
#endif

    int kpu_done()
    {
        kpu_.interrupt_clear.data.calc_done_int = 1;
        kpu_.interrupt_clear.data.layer_cfg_almost_empty_int = 1;
        kpu_.interrupt_clear.data.layer_cfg_almost_full_int = 1;
        kpu_.interrupt_clear.data.reserved = 0;

        kpu_.interrupt_mask.data.calc_done_int = 1;
        kpu_.interrupt_mask.data.layer_cfg_almost_empty_int = 1;
        kpu_.interrupt_mask.data.layer_cfg_almost_full_int = 1;
        kpu_.interrupt_mask.data.reserved = 0;

#if KPU_DEBUG
        uint32_t cnt_layer_id = ctx_.current_layer - 1;
        gettimeofday(&time_, NULL);
        if (total_time_ != 0)
        {
            uint64_t layer_time = (time_.tv_sec -last_time_.tv_sec) * 1000*1000 + (time_.tv_usec - last_time_.tv_usec);
            printf("layer %d [%s]: %f ms\n", cnt_layer_id, str_layer_type(last_layer_type_), layer_time / 1000.0);
            total_time_ += layer_time;
        }
        printf("Model: %f ms\n", total_time_ / 1000.0);
#endif

        done_flag_ = 1;
        return 0;
    }

    int ai_step()
    {
        uint32_t cnt_layer_id = ctx_.current_layer++;
        const uint8_t *layer_body = ctx_.current_body;
        const kpu_model_layer_header_t *cnt_layer_header = ctx_.layer_headers + cnt_layer_id;
        ctx_.current_body += cnt_layer_header->body_size;

#if KPU_DEBUG
        uint64_t layer_time;
        gettimeofday(&time_, NULL);
        layer_time = (time_.tv_sec -last_time_.tv_sec) * 1000*1000 + (time_.tv_usec - last_time_.tv_usec);
        if(total_time_ == 0)
            printf("DMA INPUT: %f ms\n", layer_time / 1000.0);
        else
            printf("layer %d [%s]: %f ms\n", cnt_layer_id - 1, str_layer_type(last_layer_type_), layer_time / 1000.0);
        total_time_ += layer_time;

        last_layer_type_ = cnt_layer_header->type;
        gettimeofday(&last_time_, NULL);
#endif
        switch (cnt_layer_header->type)
        {
            case KL_ADD:
                kpu_add((const kpu_model_add_layer_argument_t *)layer_body);
                break;
            case KL_QUANTIZED_ADD:
                kpu_quantized_add((const kpu_model_quant_add_layer_argument_t *)layer_body);
                break;
            case KL_GLOBAL_AVERAGE_POOL2D:
                kpu_global_average_pool2d((const kpu_model_gap2d_layer_argument_t *)layer_body);
                break;
            case KL_QUANTIZED_MAX_POOL2D:
                kpu_quantized_max_pool2d((const kpu_model_quant_max_pool2d_layer_argument_t *)layer_body);
                break;
            case KL_QUANTIZE:
                kpu_quantize((const kpu_model_quantize_layer_argument_t *)layer_body);
                break;
            case KL_DEQUANTIZE:
                kpu_dequantize((const kpu_model_dequantize_layer_argument_t *)layer_body);
                break;
            case KL_REQUANTIZE:
                kpu_requantize((const kpu_model_requantize_layer_argument_t *)layer_body);
                break;
            case KL_L2_NORMALIZATION:
                kpu_l2_normalization((const kpu_model_l2_norm_layer_argument_t *)layer_body);
                break;
            case KL_SOFTMAX:
                kpu_softmax((const kpu_model_softmax_layer_argument_t *)layer_body);
                break;
            case KL_CONCAT:
            case KL_QUANTIZED_CONCAT:
                kpu_concat((const kpu_model_concat_layer_argument_t *)layer_body);
                break;
            case KL_K210_CONV:
                kpu_conv((const kpu_model_conv_layer_argument_t *)layer_body);
                return 0;
            case KL_K210_ADD_PADDING:
                kpu_add_padding((const kpu_model_add_padding_layer_argument_t *)layer_body);
                break;
            case KL_K210_REMOVE_PADDING:
                kpu_remove_padding((const kpu_model_remove_padding_layer_argument_t *)layer_body);
                break;
            case KL_K210_UPLOAD:
                kpu_upload((const kpu_model_upload_layer_argument_t *)layer_body);
                break;
            default:
                assert(!"Layer is not supported.");
        }

        if (cnt_layer_id != (ctx_.layers_length - 1))
        {
            return 1;
        }
        else
        {
            kpu_done();
            return 0;
        }
    }

    void ai_step_not_isr()
    {
        portENTER_CRITICAL();
        ai_step();
        vPortExitCritical();
    }

private:
    volatile kpu_config_t &kpu_;
    sysctl_clock_t clock_;
    sysctl_dma_select_t dma_req_;
    SemaphoreHandle_t free_mutex_;
    uintptr_t dma_ch_;
    SemaphoreHandle_t completion_event_;
    uint8_t done_flag_ = 0;
    kpu_model_context_t ctx_;
#if KPU_DEBUG
    struct timeval time_;
    struct timeval last_time_;
    uint64_t total_time_ = 0;
    uint32_t last_layer_type_;
#endif
};

static k_kpu_driver dev0_driver(AI_BASE_ADDR, SYSCTL_CLOCK_AI, SYSCTL_DMA_SELECT_AI_RX_REQ);

driver &g_kpu_driver_kpu0 = dev0_driver;
