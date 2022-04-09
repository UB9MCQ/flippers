#include <furi.h>
#include <furi_hal_uart.h>
#include <stm32_hal_legacy.h>
#include <gui/gui.h>
#include "esp_deauch.h"

int binflash;
int uart_no = __USART1_CLK_DISABLE;
typedef struct {
    Gui* gui;
    ViewPort* view_port;
} CounterApp;

void esp_deauch_app_free(CounterApp* app) {
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close("gui");
    free(app);
}

CounterApp* counter_app_alloc() {
    CounterApp* app = furi_alloc(sizeof(CounterApp));
    app->view_port = view_port_alloc();
    app->gui = furi_record_open("gui");
    furi_hal_uart_init(1,115200);

    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    return app;
}
void esn_esp()
{

    ETS_UART_INTR_ATTACH(uart0_rx_intr_handler,  &(USART_InitStruct.PrescalerValue));
    PIN_PULLUP_DIS(PERIPHS_IO_MUX_U0TXD_U);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, FUNC_U0RTS);
  

  uart_div_modify(uart_no, UART_CLK_FREQ / (USART_InitStruct.BaudRate));

  if (uart_no == UART0)  
	  WRITE_PERI_REG(UART_CONF0(uart_no), CALC_UARTMODE(EIGHT_BITS, NONE_BITS, ONE_STOP_BIT));
  else
	  WRITE_PERI_REG(UART_CONF0(uart_no), CALC_UARTMODE(USART_InitStruct.data_bits, USART_InitStruct.parity, USART_InitStruct.stop_bits));


  SET_PERI_REG_MASK(UART_CONF0(uart_no), UART_RXFIFO_RST | UART_TXFIFO_RST);
  CLEAR_PERI_REG_MASK(UART_CONF0(uart_no), UART_RXFIFO_RST | UART_TXFIFO_RST);

 
  WRITE_PERI_REG(UART_CONF1(uart_no),
                ((USART_InitStruct.rcv_buff.TrigLvl & UART_RXFIFO_FULL_THRHD) << UART_RXFIFO_FULL_THRHD_S) |
                 ((96 & UART_TXFIFO_EMPTY_THRHD) << UART_TXFIFO_EMPTY_THRHD_S) |
                 UART_RX_FLOW_EN);
  if (uart_no == UART0)
  {

    WRITE_PERI_REG(UART_CONF1(uart_no),
                   ((0x10 & UART_RXFIFO_FULL_THRHD) << UART_RXFIFO_FULL_THRHD_S) |
                   ((0x10 & UART_RX_FLOW_THRHD) << UART_RX_FLOW_THRHD_S) |
                   UART_RX_FLOW_EN |
                   (0x02 & UART_RX_TOUT_THRHD) << UART_RX_TOUT_THRHD_S |
                   UART_RX_TOUT_EN);
    SET_PERI_REG_MASK(UART_INT_ENA(uart_no), UART_RXFIFO_TOUT_INT_ENA |
                      UART_FRM_ERR_INT_ENA);
  }
  else
  {
    WRITE_PERI_REG(UART_CONF1(uart_no),
                   ((UartDev.rcv_buff.TrigLvl & UART_RXFIFO_FULL_THRHD) << UART_RXFIFO_FULL_THRHD_S));
  }


  WRITE_PERI_REG(UART_INT_CLR(uart_no), 0xffff);

  SET_PERI_REG_MASK(UART_INT_ENA(uart_no), UART_RXFIFO_FULL_INT_ENA);


  if(UART_FRM_ERR_INT_ST == (READ_PERI_REG(UART_INT_ST(uart_no)) & UART_FRM_ERR_INT_ST))
  {
    os_printf("FRM_ERR\r\n");
    WRITE_PERI_REG(UART_INT_CLR(uart_no), UART_FRM_ERR_INT_CLR);
  }

  if(UART_RXFIFO_FULL_INT_ST == (READ_PERI_REG(UART_INT_ST(uart_no)) & UART_RXFIFO_FULL_INT_ST))
  {

    ETS_UART_INTR_DISABLE();

  }
  else if(UART_RXFIFO_TOUT_INT_ST == (READ_PERI_REG(UART_INT_ST(uart_no)) & UART_RXFIFO_TOUT_INT_ST))
  {
    ETS_UART_INTR_DISABLE();
    os_printf("stat:%02X",*(uint8_t *)UART_INT_ENA(uart_no));
    furi_hal_uart_deinit(0);

    WRITE_PERI_REG(UART_INT_CLR(uart_no), UART_RXFIFO_TOUT_INT_CLR);
    os_printf("rx time over\r\n");
   while (READ_PERI_REG(UART_STATUS(uart_no)) & (UART_RXFIFO_CNT << UART_RXFIFO_CNT_S))
   {
      os_printf("process recv\r\n");
      at_recvTask();
      binflash = READ_PERI_REG(UART_FIFO(uart_no)) & 0xFF;
      system_os_post(UART_FIFO(uart_no), NULL, binflash);
    }
  }
}


if READ_PERI_REG(UART_STATUS(uart_no)) & (UART_RXFIFO_CNT << UART_RXFIFO_CNT_S))
{
binflash = READ_PERI_REG(UART_FIFO(uart_no)) & 0xFF;
t_recvTask();
*(binflash->pWritePos) = binflash;

 ystem_os_post(at_recvTaskPrio, NULL, RcvChar);


   if (RcvChar == '\r')
    {
     pRxBuff->BuffState = WRITE_OVER;
    }

    pRxBuff->pWritePos++;

    if (pRxBuff->pWritePos == (pRxBuff->pRcvMsgBuff + RX_BUFF_SIZE))
    {
      pRxBuff->pWritePos = pRxBuff->pRcvMsgBuff ;
    }
  
}
}
int32_t esp_deauch(void* p) {
    CounterApp* app = esp_deauch_app_alloc();

    delay(2000);

    esp_deauch_app_free(app);
    return 0;
}