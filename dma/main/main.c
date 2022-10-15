// original code, https://moliam.space/2021/11/17/experience/esp32/adc_dma/




//by xiaolaba
#include <stdio.h>
#include "soc/spi_reg.h"  		// SPI_DMA_INT_ST_REG
#include "soc/periph_defs.h"    // ETS_SPI3_DMA_INTR_SOURCE, esp32-s2 only



#include "esp_system.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/adc.h"
#include "driver/dac.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include "unity.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "adc.h"
#include "soc/adc_periph.h"

#include "soc/system_reg.h"
#include "soc/lldesc.h"
#include "adc.h"

//#include "sysCfg.h"	//by xiaolaba
#include "esp_log.h"



static const char *TAG = "adc";
#define debug_i(format,...)			ESP_LOGI(TAG,format,##__VA_ARGS__)
#define debug_w(format,...)			ESP_LOGW(TAG,format,##__VA_ARGS__)
#define debug_e(format,...)			ESP_LOGE(TAG,format,##__VA_ARGS__)


uint8_t link_buf[SAR_SIMPLE_NUM*2*2] = {0};
static lldesc_t dma1 = {0};
static QueueHandle_t que_adc = NULL;
static adc_dma_event_t adc_evt;

/** ADC-DMA ISR handler. */
static IRAM_ATTR void adc_dma_isr(void *arg)
{
    uint32_t int_st = REG_READ(SPI_DMA_INT_ST_REG(3));
    int task_awoken = pdFALSE;
    REG_WRITE(SPI_DMA_INT_CLR_REG(3), int_st);
    if (int_st & SPI_IN_SUC_EOF_INT_ST_M) {
        adc_evt.int_msk = int_st;
        xQueueSendFromISR(que_adc, &adc_evt, &task_awoken);
    }
    if (int_st & SPI_IN_DONE_INT_ST) {
        adc_evt.int_msk = int_st;
        xQueueSendFromISR(que_adc, &adc_evt, &task_awoken);
    }

    if (task_awoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

 uint32_t adc_dma_linker_init(void)
{
    dma1 = (lldesc_t) {
        .size = SAR_SIMPLE_NUM*2*2,  
        .owner = 1,
        .buf = &link_buf[0],
        .qe.stqe_next = NULL,
    };
    return (uint32_t)&dma1;
}

typedef struct adc_dac_dma_isr_handler_ {
    uint32_t mask;
    intr_handler_t handler;
    void* handler_arg;
    SLIST_ENTRY(adc_dac_dma_isr_handler_) next;
} adc_dac_dma_isr_handler_t;

static SLIST_HEAD(adc_dac_dma_isr_handler_list_, adc_dac_dma_isr_handler_) s_adc_dac_dma_isr_handler_list =
        SLIST_HEAD_INITIALIZER(s_adc_dac_dma_isr_handler_list);
portMUX_TYPE s_isr_handler_list_lock = portMUX_INITIALIZER_UNLOCKED;
static intr_handle_t s_adc_dac_dma_isr_handle;

static IRAM_ATTR void adc_dac_dma_isr_default(void* arg)
{
    uint32_t status = REG_READ(SPI_DMA_INT_ST_REG(3));
    adc_dac_dma_isr_handler_t* it;
    portENTER_CRITICAL_ISR(&s_isr_handler_list_lock);
    SLIST_FOREACH(it, &s_adc_dac_dma_isr_handler_list, next) {
        if (it->mask & status) {
            portEXIT_CRITICAL_ISR(&s_isr_handler_list_lock);
            (*it->handler)(it->handler_arg);
            portENTER_CRITICAL_ISR(&s_isr_handler_list_lock);
        }
    }
    portEXIT_CRITICAL_ISR(&s_isr_handler_list_lock);
    REG_WRITE(SPI_DMA_INT_CLR_REG(3), status);
}

static esp_err_t adc_dac_dma_isr_ensure_installed(void)
{
    esp_err_t err = ESP_OK;
    portENTER_CRITICAL(&s_isr_handler_list_lock);
    if (s_adc_dac_dma_isr_handle) {
        goto out;
    }
    REG_WRITE(SPI_DMA_INT_ENA_REG(3), 0);
    REG_WRITE(SPI_DMA_INT_CLR_REG(3), UINT32_MAX);
    err = esp_intr_alloc(ETS_SPI3_DMA_INTR_SOURCE, 0, &adc_dac_dma_isr_default, NULL, &s_adc_dac_dma_isr_handle);
    if (err != ESP_OK) {
        goto out;
    }

out:
    portEXIT_CRITICAL(&s_isr_handler_list_lock);
    return err;
}
esp_err_t adc_dac_dma_isr_register(intr_handler_t handler, void* handler_arg, uint32_t intr_mask)
{
    esp_err_t err = adc_dac_dma_isr_ensure_installed();
    if (err != ESP_OK) {
        return err;
    }

    adc_dac_dma_isr_handler_t* item = malloc(sizeof(*item));
    if (item == NULL) {
        return ESP_ERR_NO_MEM;
    }
    item->handler = handler;
    item->handler_arg = handler_arg;
    item->mask = intr_mask;
    portENTER_CRITICAL(&s_isr_handler_list_lock);
    SLIST_INSERT_HEAD(&s_adc_dac_dma_isr_handler_list, item, next);
    portEXIT_CRITICAL(&s_isr_handler_list_lock);
    return ESP_OK;
}

void adc_dac_dma_linker_start( void *dma_addr, uint32_t int_msk)
{
    REG_SET_BIT(DPORT_PERIP_CLK_EN_REG, DPORT_APB_SARADC_CLK_EN_M);
    REG_SET_BIT(DPORT_PERIP_CLK_EN_REG, DPORT_SPI3_DMA_CLK_EN_M);
    REG_SET_BIT(DPORT_PERIP_CLK_EN_REG, DPORT_SPI3_CLK_EN);
    REG_CLR_BIT(DPORT_PERIP_RST_EN_REG, DPORT_SPI3_DMA_RST_M);
    REG_CLR_BIT(DPORT_PERIP_RST_EN_REG, DPORT_SPI3_RST_M);
    REG_WRITE(SPI_DMA_INT_CLR_REG(3), 0xFFFFFFFF);
    REG_WRITE(SPI_DMA_INT_ENA_REG(3), int_msk | REG_READ(SPI_DMA_INT_ENA_REG(3)));

    REG_SET_BIT(SPI_DMA_IN_LINK_REG(3), SPI_INLINK_STOP);
    REG_CLR_BIT(SPI_DMA_IN_LINK_REG(3), SPI_INLINK_START);
    SET_PERI_REG_BITS(SPI_DMA_IN_LINK_REG(3), SPI_INLINK_ADDR, (uint32_t)dma_addr, 0);
    REG_SET_BIT(SPI_DMA_CONF_REG(3), SPI_IN_RST);
    REG_CLR_BIT(SPI_DMA_CONF_REG(3), SPI_IN_RST);
    REG_CLR_BIT(SPI_DMA_IN_LINK_REG(3), SPI_INLINK_STOP);
    REG_SET_BIT(SPI_DMA_IN_LINK_REG(3), SPI_INLINK_START);
}

esp_err_t adc_get_value_group(uint16_t *buf,const int num)
{
    adc_dma_event_t evt;

    adc_dac_dma_linker_start((void *)&dma1, SPI_IN_SUC_EOF_INT_ENA);
    adc_digi_start();
     while (1) {
        xQueueReceive(que_adc, &evt, 10 / portTICK_RATE_MS);
        if (evt.int_msk & SPI_IN_SUC_EOF_INT_ENA) {
            break;
        }
    }
    adc_digi_stop();
    uint16_t *buf1 = (uint16_t *)link_buf;
    for(int i=0;i<num;i++){
        buf[i] = (buf1[i] & 0xFFF);
    }
    return ESP_OK;
}

int adc_dig_bsp_init()
{
    adc_digi_config_t config = {
        .conv_limit_en = false,
        .conv_limit_num = 0,
        .interval = 40,
        .dig_clk.use_apll = 0,  // APB clk
        .dig_clk.div_num = 9,
        .dig_clk.div_b = 0,
        .dig_clk.div_a = 0,
        .dma_eof_num = SAR_SIMPLE_NUM*2,
    };
    adc_digi_pattern_table_t adc1_patt = {0};
    config.adc1_pattern_len = 1;
    config.adc1_pattern = &adc1_patt;
    adc1_patt.atten = ADC_ATTEN_11db;
    adc1_patt.channel = ADC1_CHANNEL_8;  
    adc_gpio_init(ADC_UNIT_1,ADC1_CHANNEL_8 );
    config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    config.format = ADC_DIGI_FORMAT_12BIT;
    adc_digi_controller_config(&config);
    if (que_adc == NULL) {
        que_adc = xQueueCreate(5, sizeof(adc_dma_event_t));
    } else {
        xQueueReset(que_adc);
    }
    uint32_t int_mask = SPI_IN_SUC_EOF_INT_ENA;
    uint32_t dma_addr = adc_dma_linker_init();
    adc_dac_dma_isr_register(adc_dma_isr, NULL, int_mask);
    //先采集一次
    uint16_t buf[SAR_SIMPLE_NUM];
    adc_dac_dma_linker_start((void *)dma_addr, int_mask);
    adc_get_value_group(buf, 6);
    
    return 0;
}




//by xiaolaba, testing for code gen only, no hardware used for real rig / code run
void app_main(void)
{
    printf("Hello world!\n");
	adc_dma_linker_init();
	adc_dig_bsp_init();
}