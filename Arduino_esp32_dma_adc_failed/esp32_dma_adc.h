//��Ȩ����������Ϊ����ԭ�����£���ѭ CC 4.0 BY-SA ��ȨЭ�飬ת���븽��ԭ�ĳ������Ӻͱ�������
//�������ӣ�https://blog.csdn.net/weixin_44529321/article/details/115082805



#pragma once 

#include "stdint.h"

#define SAR_SIMPLE_NUM  12 //��Ҫ������bug

typedef struct dma_msg {
    uint32_t int_msk;
    uint8_t *data;
    uint32_t data_len;
} adc_dma_event_t;//���ڶ��е��¼�
//�ⲿ��ȡ��ADCֵ��API
esp_err_t adc_get_value_group(uint16_t *buf,const int num);
//�ⲿ��ʼ������
int adc_dig_bsp_init(void);
