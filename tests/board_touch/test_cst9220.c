// SPDX-License-Identifier: Apache-2.0
/* Compile the unchanged pinned decoder and production observation bridge.
 * Only panel IO, RTOS, GPIO and the generic touch facade are substituted. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../firmware/managed_components/espressif__esp_lcd_touch_cst9220/esp_lcd_touch_cst9220.c"
#include "../../firmware/main/board_touch_mosaico.c"
#include "interaction_multitouch.h"

static uint8_t wire[15];
static unsigned reads, acknowledgements;
static board_touch_snapshot_t observed;
static esp_err_t raw_read(esp_lcd_panel_io_t *io, int cmd, void *data, size_t bytes)
{ (void)io; assert(cmd==-1 && bytes==15); ++reads; memcpy(data,wire,bytes); return ESP_OK; }
static esp_lcd_panel_io_t panel={.rx_param=raw_read};
static cst9220_touch_t controller;
static lv_indev_t input;
static esp_lv_adapter_touch_callbacks_t callbacks;
const char *esp_err_to_name(esp_err_t error) { (void)error; return "boundary"; }
void raw_touch_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
int64_t esp_timer_get_time(void) { return (int64_t)reads * 10000; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) { (void)pin; (void)level; return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t pin) { (void)pin; return ESP_OK; }
esp_err_t gpio_intr_disable(gpio_num_t pin) { (void)pin; return ESP_OK; }
esp_err_t gpio_intr_enable(gpio_num_t pin) { (void)pin; return ESP_OK; }
esp_err_t gpio_isr_handler_remove(gpio_num_t pin) { (void)pin; return ESP_OK; }
esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t io,int cmd,void *data,size_t bytes)
{ return io->rx_param(io,cmd,data,bytes); }
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io,int cmd,const void *data,size_t bytes)
{
    assert(io==&panel && cmd==0xd0 && data);
    const uint8_t *p=data; assert(p[0]==0);
    if(bytes==2) { assert(p[1]==0xab); ++acknowledgements; } else assert(bytes==1);
    return ESP_OK;
}
esp_err_t esp_lcd_touch_register_interrupt_callback(esp_lcd_touch_handle_t touch, esp_lcd_touch_interrupt_callback_t callback)
{ (void)touch; (void)callback; return ESP_OK; }
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t touch) { return touch->read_data(touch); }
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t touch, esp_lcd_touch_point_data_t *data,uint8_t *count,uint8_t capacity)
{
    *count=touch->data.points < capacity ? touch->data.points : capacity;
    memcpy(data,touch->data.coords,*count*sizeof(*data)); return ESP_OK;
}
lv_indev_t *bsp_display_get_input_dev(void) { return &input; }
esp_err_t esp_lv_adapter_set_touch_callbacks(lv_indev_t *device,const esp_lv_adapter_touch_callbacks_t *set)
{ assert(device==&input); callbacks=*set; return ESP_OK; }
static bool observe(const board_touch_snapshot_t *sample) { observed=*sample; return true; }
static void pair_report(void)
{ memset(wire,0,sizeof wire); wire[0]=0x36; wire[1]=10; wire[2]=10; wire[5]=2; wire[6]=0xab; wire[7]=0x76; wire[8]=20; wire[9]=10; }
static interaction_multitouch_result_t recognize(interaction_multitouch_t *model, bool held)
{
    interaction_contact_snapshot_t sample={.sequence=observed.sequence,.timestamp_ms=observed.timestamp_ms,
        .epoch=1,.ui_revision=1,.supported=true,.fresh=true,.valid=observed.valid,
        .overflow=observed.overflow,.count=observed.count};
    for(unsigned i=0;i<sample.count;++i) sample.contacts[i]=(interaction_contact_t){.id=observed.contacts[i].id,
        .x=observed.contacts[i].x,.y=observed.contacts[i].y};
    const interaction_multitouch_context_t context={.epoch=1,.ui_revision=1,.enabled=true,.drag_held=held};
    return interaction_multitouch_input(model,&sample,&context,sample.timestamp_ms);
}
int main(void)
{
    controller.base.io=&panel; controller.base.read_data=cst9220_read_data;
    controller.protocol=CST9220_PROTOCOL_HYN212;
    assert(board_touch_observe(observe)==ESP_OK);
    for(unsigned bad=0;bad<2;++bad) for(unsigned held=0;held<2;++held) {
        interaction_multitouch_t model; interaction_multitouch_init(&model);
        esp_lcd_touch_point_data_t point; uint8_t count=0;
        pair_report();
        assert(callbacks.custom_touch_read(&controller.base,&point,&count,1,NULL)==ESP_OK && count==1);
        assert(observed.valid && observed.count==2); (void)recognize(&model,false);
        if(bad==0) wire[6]=0; else wire[5]=3;
        assert(callbacks.custom_touch_read(&controller.base,&point,&count,1,NULL)==ESP_ERR_INVALID_STATE && !count);
        assert(!observed.valid && observed.overflow==(bad==1));
        interaction_multitouch_result_t result=recognize(&model,held!=0);
        assert(!result.right_click && !result.scroll_y && result.consume_pointer && result.release_required==(held!=0));
        assert(reads==acknowledgements); /* Invalid report still receives its actual ACK. */
    }
    puts("PASS: actual CST9220 decoder preserves invalid-header/overflow cancellation without extra reads or lost ACKs");
    return 0;
}
