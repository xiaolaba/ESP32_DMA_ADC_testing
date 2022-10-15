#pragma once 

#include "stdint.h"

#define SAR_SIMPLE_NUM  12 //需要采样的bug

typedef struct dma_msg {
    uint32_t int_msk;
    uint8_t *data;
    uint32_t data_len;
} adc_dma_event_t;//用于队列的事件
//外部获取的ADC值的API
esp_err_t adc_get_value_group(uint16_t *buf,const int num);
//外部初始化函数
int adc_dig_bsp_init(void);