/** @file
 *
 * @defgroup ble_sdk_uart_over_ble_main main.c
 * @{
 * @ingroup  ble_sdk_app_nus_eval
 * @brief    UART over BLE application main file.
 *
 * This file contains the source code for a sample application that uses the Nordic UART service.
 * This application uses the @ref srvlib_conn_params module.
 */



#include <stdint.h>
#include <string.h>
#include "nordic_common.h"
#include "nrf.h"
#include "ble_hci.h"
#include "ble_advdata.h"
#include "ble_advertising.h"
#include "ble_conn_params.h"
#include "nrf_sdh.h"
#include "nrf_sdh_soc.h"
#include "nrf_sdh_ble.h"
#include "nrf_ble_gatt.h"
#include "app_timer.h"
#include "ble_nus.h"
#include "app_uart.h"
#include "app_util_platform.h"
#include "bsp_btn_ble.h"
#include "nrf_drv_timer.h"
#include "nrf_drv_wdt.h"

#if defined (UART_PRESENT)
#include "nrf_uart.h"
#endif
#if defined (UARTE_PRESENT)
#include "nrf_uarte.h"
#endif

#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"

#define HARDWARE_NUMBER					"HW_1.2"
#define SOFTWARE_NUMBER					"SW_1.0.3"
#define FIRMWARE_NUMBER					"FW_14.2.0"

#define APP_BLE_CONN_CFG_TAG            1                                           /**< A tag identifying the SoftDevice BLE configuration. */

#define APP_FEATURE_NOT_SUPPORTED       BLE_GATT_STATUS_ATTERR_APP_BEGIN + 2        /**< Reply when unsupported features are requested. */

#define DEVICE_NAME                     "Core52_BLE_UART"                               /**< Name of device. Will be included in the advertising data. */
#define NUS_SERVICE_UUID_TYPE           BLE_UUID_TYPE_VENDOR_BEGIN                  /**< UUID type for the Nordic UART Service (vendor specific). */

#define APP_BLE_OBSERVER_PRIO           3                                           /**< Application's BLE observer priority. You shouldn't need to modify this value. */

#define APP_ADV_INTERVAL                MSEC_TO_UNITS(20, UNIT_0_625_MS)			/**< The advertising interval (in units of 0.625 ms. This value corresponds to 40 ms). */
#define APP_ADV_TIMEOUT_IN_SECONDS      0											/**< The advertising timeout (in units of seconds). */

#define MIN_CONN_INTERVAL               MSEC_TO_UNITS(20, UNIT_1_25_MS)             /**< Minimum acceptable connection interval (20 ms), Connection interval uses 1.25 ms units. */
#define MAX_CONN_INTERVAL               MSEC_TO_UNITS(75, UNIT_1_25_MS)             /**< Maximum acceptable connection interval (75 ms), Connection interval uses 1.25 ms units. */
#define SLAVE_LATENCY                   0                                           /**< Slave latency. */
#define CONN_SUP_TIMEOUT                MSEC_TO_UNITS(4000, UNIT_10_MS)             /**< Connection supervisory timeout (4 seconds), Supervision Timeout uses 10 ms units. */
#define FIRST_CONN_PARAMS_UPDATE_DELAY  APP_TIMER_TICKS(5000)                       /**< Time from initiating event (connect or start of notification) to first time sd_ble_gap_conn_param_update is called (5 seconds). */
#define NEXT_CONN_PARAMS_UPDATE_DELAY   APP_TIMER_TICKS(30000)                      /**< Time between each call to sd_ble_gap_conn_param_update after the first call (30 seconds). */
#define MAX_CONN_PARAMS_UPDATE_COUNT    3                                           /**< Number of attempts before giving up the connection parameter negotiation. */

#define DEAD_BEEF                       0xDEADBEEF                                  /**< Value used as error code on stack dump, can be used to identify stack location on stack unwind. */

#define UART_TX_BUF_SIZE                256                                         /**< UART TX buffer size. */
#define UART_RX_BUF_SIZE                256                                         /**< UART RX buffer size. */

#define WDT_FEED_INTERVALY				APP_TIMER_TICKS(500)						/**< WDT feed interval. */

#define UART_RX_TIMEOUT_INTERVAL		3											/**< uart rx timeout interval(ms). */
const nrf_drv_timer_t TIMER_UART_RX = NRF_DRV_TIMER_INSTANCE(2);					/**< Timer ID: Timer2. */											
static uint8_t 	UART_RX_BUF[UART_RX_BUF_SIZE] = {0};					  			/**< uart receive buffer. */	
static uint16_t UART_RX_STA = 0;

static uint8_t 	device_name[15] = "Core52_BLE_UART";

APP_TIMER_DEF(wdt_feed_timer_id);													/**< WDT feed delay timer instance. */
BLE_NUS_DEF(m_nus);                                                                 /**< BLE NUS service instance. */
NRF_BLE_GATT_DEF(m_gatt);                                                           /**< GATT module instance. */
BLE_ADVERTISING_DEF(m_advertising);                                                 /**< Advertising module instance. */

nrf_drv_wdt_channel_id m_channel_id;	// wdt channel id

static uint16_t adv_interval = APP_ADV_INTERVAL;

static uint16_t   m_conn_handle          = BLE_CONN_HANDLE_INVALID;                 /**< Handle of the current connection. */
static uint16_t   m_ble_nus_max_data_len = BLE_GATT_ATT_MTU_DEFAULT - 3;            /**< Maximum length of data (in bytes) that can be transmitted to the peer by the Nordic UART service module. */
static ble_uuid_t m_adv_uuids[]          =                                          /**< Universally unique service identifier. */
{
    {BLE_UUID_NUS_SERVICE, NUS_SERVICE_UUID_TYPE}
};


typedef enum
{
	BLE_STATUS_DISCONNECTED = 0,// ble disconnected
	BLE_STATUS_CONNECTED		// ble connected
} ble_status_t;

static ble_status_t ble_status = BLE_STATUS_DISCONNECTED;


typedef struct ble_send_msg_tag{ 
	uint16_t start;		// Data Start Offset
	uint16_t max_len;	// Data Length
	uint8_t  *pdata; 	// Pointer to Data
}ble_send_msg_t; 

ble_send_msg_t g_send_msg;


static uint32_t uart_init(uint32_t baudrate);
static void advertising_init(void);

/**@brief Function for the BLE send data which received by uart.
 *
 * @details This function will unpacking the uart received data,
 *          each packet at most 20 Bytes. 
 *			Then send the unpacket data over BLE until the
 *			ble_nus_string_send not success or send fineshed.
 *  
 * @return NRF_SUCCESS on success, otherwise an error code.
 */
static uint32_t send_data(void)
{
	uint16_t temp_len; 
	uint16_t dif_value; 
	uint32_t err_code = NRF_SUCCESS;

	uint8_t *pdata = g_send_msg.pdata; 
	uint16_t start = g_send_msg.start; 
	uint16_t max_len = g_send_msg.max_len; 

	do { 
		dif_value = max_len - start; 
		temp_len = (dif_value > m_ble_nus_max_data_len) ? m_ble_nus_max_data_len : dif_value; // get min data length

		err_code = ble_nus_string_send(&m_nus, pdata + start, &temp_len); 
		
		NRF_LOG_INFO("BLE notify:");
		NRF_LOG_HEXDUMP_INFO(pdata + start, temp_len);	
		
		if (NRF_SUCCESS == err_code)
		{ 
			start += temp_len;
		}
	} while((NRF_SUCCESS == err_code) && ((max_len - start) > 0)); 

	g_send_msg.start = start;
	
	return err_code; 
}


/**@brief Function for the first time BLE send data.
 * 
 * @details The first time, BLE at most send 7 packets.
 *			Fuction return should be handle at other funchtion , and return 
 *			NRF_ERROR_BUSY and BLE_ERROR_NO_TX_BUFFERS should be regard as normal result,
 *			bacause no start offset changes.
 *
 * @param[in] pdata  Data to be sent.
 * @param[in] len    Length of the data. Amount of sent bytes.
 *
 * @return NRF_SUCCESS on success, otherwise an error code. 
 */
static uint32_t ble_send_data(uint8_t *pdata, uint16_t len)
{ 
	if((NULL == pdata) || (len <= 0))
	{
		return NRF_ERROR_INVALID_PARAM;
	}

	uint32_t err_code = NRF_SUCCESS; 
	g_send_msg.start = 0; 
	g_send_msg.max_len = len; 
	g_send_msg.pdata = pdata; 

	err_code = send_data(); 
	return err_code;
}


/**@brief Function for the second time BLE send data.
 * 
 * @details This function will process BLE send data until finished.
 *
 * @return NRF_SUCCESS on success, otherwise an error code. 
 */
static uint32_t ble_send_more_data(void)
{ 
	uint32_t err_code; 
	uint16_t dif_value; 

	dif_value = g_send_msg.max_len - g_send_msg.start; 
	if((0 == dif_value) || (NULL == g_send_msg.pdata))
	{
		return NRF_SUCCESS;  // After the completion of the data, return
	}

	err_code = send_data();
	return err_code; 
}


/**@brief Function for assert macro callback.
 *
 * @details This function will be called in case of an assert in the SoftDevice.
 *
 * @warning This handler is an example only and does not fit a final product. You need to analyse
 *          how your product is supposed to react in case of Assert.
 * @warning On assert from the SoftDevice, the system can only recover on reset.
 *
 * @param[in] line_num    Line number of the failing ASSERT call.
 * @param[in] p_file_name File name of the failing ASSERT call.
 */
void assert_nrf_callback(uint16_t line_num, const uint8_t * p_file_name)
{
    app_error_handler(DEAD_BEEF, line_num, p_file_name);
}


/**@brief Function for convert Bluetooth MAC address to string.
 *
 * @param[in] pAddr   Bluetooth MAC address.
 * 
 * @return BD address as a string.
 */
char *Util_convertBdAddr2Str(uint8_t *pAddr)
{
  uint8_t     charCnt;
  char        hex[] = "0123456789ABCDEF";
  static char str[(BLE_GAP_ADDR_LEN << 1)+1];
  char        *pStr = str;

  // Start from end of addr
  pAddr += BLE_GAP_ADDR_LEN;

  for (charCnt = BLE_GAP_ADDR_LEN; charCnt > 0; charCnt--)
  {
    *pStr++ = hex[*--pAddr >> 4];
    *pStr++ = hex[*pAddr & 0x0F];
  }

  pStr = NULL;

  return str;
}


/**@brief Function for wdt feed timers.
 *
 * @details Starts application wdt feed timers.
 */
static void wdt_feed_timers_start(void)
{
	ret_code_t err_code;

    err_code = app_timer_start(wdt_feed_timer_id, WDT_FEED_INTERVALY, NULL);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for the GAP initialization.
 *
 * @details This function will set up all the necessary GAP (Generic Access Profile) parameters of
 *          the device. It also sets the permissions and appearance.
 */
static void gap_params_init(void)
{
    uint32_t                err_code;
    ble_gap_conn_params_t   gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);
	
	// device name : BLE_UART + MAC Address
	// Get BLE address.
	ble_gap_addr_t device_addr;
    #if (NRF_SD_BLE_API_VERSION >= 3)
        err_code = sd_ble_gap_addr_get(&device_addr);
    #else
        err_code = sd_ble_gap_address_get(&device_addr);
    #endif
    APP_ERROR_CHECK(err_code);
	
	 //memcpy(device_name + 9, Util_convertBdAddr2Str(device_addr.addr), (BLE_GAP_ADDR_LEN*2));
	
    err_code = sd_ble_gap_device_name_set(&sec_mode,
                                          (const uint8_t *) device_name,
                                          sizeof(device_name));
    APP_ERROR_CHECK(err_code);

    memset(&gap_conn_params, 0, sizeof(gap_conn_params));

    gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;
    gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;
    gap_conn_params.slave_latency     = SLAVE_LATENCY;
    gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;

    err_code = sd_ble_gap_ppcp_set(&gap_conn_params);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for handling the data from the Nordic UART Service.
 *
 * @details This function will process the data received from the Nordic UART BLE Service and send
 *          it to the UART module.
 *
 * @param[in] p_nus    Nordic UART Service structure.
 * @param[in] p_data   Data to be send to UART module.
 * @param[in] length   Length of the data.
 */
/**@snippet [Handling the data received over BLE] */
static void nus_data_handler(ble_nus_evt_t * p_evt)
{

    if (p_evt->type == BLE_NUS_EVT_RX_DATA)
    {
        uint32_t err_code;

        NRF_LOG_DEBUG("Received data from BLE NUS. Writing data on UART.");
        NRF_LOG_HEXDUMP_DEBUG(p_evt->params.rx_data.p_data, p_evt->params.rx_data.length);

        for (uint32_t i = 0; i < p_evt->params.rx_data.length; i++)
        {
            do
            {
                err_code = app_uart_put(p_evt->params.rx_data.p_data[i]);
                if ((err_code != NRF_SUCCESS) && (err_code != NRF_ERROR_BUSY))
                {
                    NRF_LOG_ERROR("Failed receiving NUS message. Error 0x%x. ", err_code);
                    APP_ERROR_CHECK(err_code);
                }
            } while (err_code == NRF_ERROR_BUSY);
        }
        if (p_evt->params.rx_data.p_data[p_evt->params.rx_data.length-1] == '\r')
        {
            while (app_uart_put('\n') == NRF_ERROR_BUSY);
        }
    }

}
/**@snippet [Handling the data received over BLE] */


/**@brief Function for check uart received data whether is AT cmd or Pass-through data.
 *
 * @param[in] pBuffer   pointer of string.
 * @param[in] length	Length of the data.
 *
 * @retval 1 AT command
 * @retval 0 Pass-through data
 */
static uint8_t AT_cmd_check_valid(uint8_t *pBuffer, uint16_t length)
{
	// check whether is AT cmd or not	
	if(length < 2) {
		return 0;
	}
			
	if(strncmp((char*)pBuffer, "AT", 2)!=0) {
		return 0;
	}

	return 1;
}


/**@brief Function for process AT command.
 *
 * @param[in] pBuffer   pointer of string.
 * @param[in] length	Length of the data.
 */
static void AT_cmd_handle(uint8_t *pBuffer, uint16_t length)
{
	ret_code_t err_code;
//	uint8_t i;
	
	// AT test: AT?\r\n
	if((length == 5) && (strncmp((char*)pBuffer, "AT?\r\n", 5) == 0))
	{
		printf("AT:OK\r\n");
	}
	
	// System soft reset: AT+RESET\r\n
	else if((length == 10) && (strncmp((char*)pBuffer, "AT+RESET\r\n", 10) == 0))
	{		
		NVIC_SystemReset();	// Restart the system by default	
	}	

	// MAC address check: AT+MAC?\r\n
	else if((length == 9) && (strncmp((char*)pBuffer, "AT+MAC?\r\n", 6) == 0))// check MAC addr
	{
		// Get BLE address.
		ble_gap_addr_t device_addr;
		#if (NRF_SD_BLE_API_VERSION >= 3)
			err_code = sd_ble_gap_addr_get(&device_addr);
		#else
			err_code = sd_ble_gap_address_get(&device_addr);
		#endif
		APP_ERROR_CHECK(err_code);

		printf("AT+MAC:%s\r\n", Util_convertBdAddr2Str(device_addr.addr));
	}
	
	// Hardware/firmware/software version check: AT+VER?\r\n
	else if((length == 9) && (strncmp((char*)pBuffer, "AT+VER?\r\n", 6) == 0))
	{
		printf("AT+VER:%s,%s,%s\r\n", HARDWARE_NUMBER, FIRMWARE_NUMBER, SOFTWARE_NUMBER);	
	}	
	
	// BLE connection status check: AT+STATUS?\r\n
	else if((length == 12) && (strncmp((char*)pBuffer, "AT+STATUS?\r\n", 12) == 0))
	{
		printf("AT+STATUS:%X\r\n", ble_status);//ble_status
	}
	
	// Disconnet connection: AT+DISCON\r\n
	else if((length == 11) && (strncmp((char*)pBuffer, "AT+DISCON\r\n", 11) == 0))
	{
		if(ble_status == BLE_STATUS_CONNECTED)
		{
			err_code = sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
			if(err_code == NRF_SUCCESS)
			{
				printf("AT+DISCON:OK\r\n");
			}
			else
			{
				printf("AT+DISCON:FAIL\r\n");
			}
			APP_ERROR_CHECK(err_code);
		}
		else
		{
			printf("AT+DISCON:ERP\r\n");
		}
	}

	// Uart baudrate setup: AT+BAUD=N\r\n, N ranges from 0 to 8
	else if((length == 11) && (strncmp((char*)pBuffer, "AT+BAUD=", 8) == 0))//baudrate setup
	{
		uint32_t uart_baudrate[9] = {
						UART_BAUDRATE_BAUDRATE_Baud4800,	// 4800
						UART_BAUDRATE_BAUDRATE_Baud9600, 	// 9600
						UART_BAUDRATE_BAUDRATE_Baud19200, 	// 19200
						UART_BAUDRATE_BAUDRATE_Baud38400, 	// 38400
						UART_BAUDRATE_BAUDRATE_Baud57600, 	// 57600
						UART_BAUDRATE_BAUDRATE_Baud115200, 	// 115200
						UART_BAUDRATE_BAUDRATE_Baud230400, 	// 230400
						UART_BAUDRATE_BAUDRATE_Baud460800,	// 460800
						UART_BAUDRATE_BAUDRATE_Baud921600};	// 921600
		if((pBuffer[8] >= '0') && (pBuffer[8] < '9'))
		{
			app_uart_close();
			err_code = uart_init(uart_baudrate[pBuffer[8] - '0']);
			if(err_code == NRF_SUCCESS)
			{
				printf("AT+BAUD:OK\r\n");
			}
			else
			{
				printf("AT+BAUD:FAIL\r\n");
			}
			APP_ERROR_CHECK(err_code);
		}
		else
		{
			printf("AT+BAUD:ERP\r\n");
		}
	}
	
	// Tx power setup: AT+TXPW=N\r\n, N ranges from 0 to 8
	else if((length == 11) && (strncmp((char*)pBuffer, "AT+TXPW=", 8) == 0))
	{
		int8_t tx_power_level[9] = {-40, -20, -16, -12, -8, -4, 0, 3, 4};
		
		if((pBuffer[8] >= '0') && (pBuffer[8] < '9'))
		{
			err_code = sd_ble_gap_tx_power_set(tx_power_level[pBuffer[8] - '0']);
			if(err_code == NRF_SUCCESS)
			{
				printf("AT+TXPW:OK\r\n");
			}
			else
			{
				printf("AT+TXPW:FAIL\r\n");
			}
			APP_ERROR_CHECK(err_code);		
		}
		else
		{
			printf("AT+TXPW:ERP\r\n");
		}
	}
	
	// Adv interval setup: AT+ADV=<adv_interval>\r\n
	else if((length >= 10) && (strncmp((char*)pBuffer, "AT+ADV=", 7) == 0))
	{
		uint32_t adv_interval_temp;
		sscanf((char*)pBuffer, "AT+ADV=%x\r\n", &adv_interval_temp);
	
		if((adv_interval_temp >= 0x0020) && (adv_interval_temp <= 0x4000))
		{
			adv_interval = adv_interval_temp;
			
			sd_ble_gap_adv_stop();

			advertising_init();
			err_code = ble_advertising_start(&m_advertising, BLE_ADV_MODE_FAST);
			if(err_code == NRF_SUCCESS)
			{
				printf("AT+ADV:OK\r\n");
			}
			else
			{
				printf("AT+ADV:FAIL\r\n");
			}
			APP_ERROR_CHECK(err_code);
		}
		else
		{
			printf("AT+ADV:ERP\r\n");
		}
	}
	
// Connection interval setup:AT+CON=<min_connect_interval>,<max_connect_interval>,<slave_latency>,<conn_sup_timeout>\r\n
	else if((length >= 16) && (strncmp((char*)pBuffer, "AT+CON=", 7) == 0))
	{
		ble_gap_conn_params_t   gap_conn_params;
		
		uint32_t min_conn_interval_tmp;	// MIN_CONN_INTERVAL
		uint32_t max_conn_interval_tmp;	// MAX_CONN_INTERVAL
		uint32_t slave_latency_tmp;		// SLAVE_LATENCY
		uint32_t conn_sup_timeout_tmp;	// CONN_SUP_TIMEOUT

		if(ble_status == BLE_STATUS_CONNECTED)
		{
			memset(&gap_conn_params, 0, sizeof(gap_conn_params));

			sscanf((char*)pBuffer, "AT+CON=%x,%x,%x,%x\r\n", 
									&min_conn_interval_tmp,	
									&max_conn_interval_tmp,	
									&slave_latency_tmp,		
									&conn_sup_timeout_tmp);	

//			printf("set connect_interval min:0x%X,max:0x%X,slave_latency:0x%X,timeout:0x%X\r\n", 
//						min_conn_interval_tmp, max_conn_interval_tmp, 
//						slave_latency_tmp, conn_sup_timeout_tmp);
		
			if((min_conn_interval_tmp >= 0x0006) 
				&& (min_conn_interval_tmp <= max_conn_interval_tmp) 
				&& (max_conn_interval_tmp <= 0x0C80) 
				&& (slave_latency_tmp <= 499) 
				&& (conn_sup_timeout_tmp >= 1) 
				&& (conn_sup_timeout_tmp <= 3200)
				&& ((conn_sup_timeout_tmp*10.0) > (1 + slave_latency_tmp)*(max_conn_interval_tmp*5.0/4)))
			{
				gap_conn_params.min_conn_interval = min_conn_interval_tmp;
				gap_conn_params.max_conn_interval = max_conn_interval_tmp;
				gap_conn_params.slave_latency = slave_latency_tmp;
				gap_conn_params.conn_sup_timeout = conn_sup_timeout_tmp;
				
				err_code = sd_ble_gap_conn_param_update(m_conn_handle, &gap_conn_params);
				if(err_code == NRF_SUCCESS)
				{
					printf("AT+CON:OK\r\n");
				}
				else
				{
					printf("AT+CON:FAIL\r\n");
				}
				APP_ERROR_CHECK(err_code);		
				

				err_code = sd_ble_gap_ppcp_get(&gap_conn_params);
				APP_ERROR_CHECK(err_code);
				
				NRF_LOG_INFO("Connect Parameter:%d,%d,%d,%d",
							min_conn_interval_tmp,
							max_conn_interval_tmp,
							slave_latency_tmp,
							conn_sup_timeout_tmp);		
			}
			else
			{
				printf("AT+CON:ERP\r\n");
			}
		}
		else
		{
			printf("AT+CON:ERP\r\n");
		}
	}
	
}

/**@brief Function for feed WDT.
 *
 * @param[in] p_context   Unused.
 */
static void wdt_feed_timer_handler(void * p_context)
{
	nrf_drv_wdt_channel_feed(m_channel_id);
}


/**
 * @brief WDT events handler.
 */
void wdt_event_handler(void)
{
    // NOTE: The max amount of time we can spend in WDT interrupt is two cycles of 32768[Hz] clock - after that, reset occurs
}


/**@brief Function for initializing services that will be used by the application.
 */
static void services_init(void)
{
    uint32_t       err_code;
    ble_nus_init_t nus_init;

    memset(&nus_init, 0, sizeof(nus_init));

    nus_init.data_handler = nus_data_handler;

    err_code = ble_nus_init(&m_nus, &nus_init);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for handling an event from the Connection Parameters Module.
 *
 * @details This function will be called for all events in the Connection Parameters Module
 *          which are passed to the application.
 *
 * @note All this function does is to disconnect. This could have been done by simply setting
 *       the disconnect_on_fail config parameter, but instead we use the event handler
 *       mechanism to demonstrate its use.
 *
 * @param[in] p_evt  Event received from the Connection Parameters Module.
 */
static void on_conn_params_evt(ble_conn_params_evt_t * p_evt)
{
    uint32_t err_code;

    if (p_evt->evt_type == BLE_CONN_PARAMS_EVT_FAILED)
    {
        err_code = sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_CONN_INTERVAL_UNACCEPTABLE);
        APP_ERROR_CHECK(err_code);
    }
}


/**@brief Function for handling errors from the Connection Parameters module.
 *
 * @param[in] nrf_error  Error code containing information about what went wrong.
 */
static void conn_params_error_handler(uint32_t nrf_error)
{
    APP_ERROR_HANDLER(nrf_error);
}


/**@brief Function for initializing the Connection Parameters module.
 */
static void conn_params_init(void)
{
    uint32_t               err_code;
    ble_conn_params_init_t cp_init;

    memset(&cp_init, 0, sizeof(cp_init));

    cp_init.p_conn_params                  = NULL;
    cp_init.first_conn_params_update_delay = FIRST_CONN_PARAMS_UPDATE_DELAY;
    cp_init.next_conn_params_update_delay  = NEXT_CONN_PARAMS_UPDATE_DELAY;
    cp_init.max_conn_params_update_count   = MAX_CONN_PARAMS_UPDATE_COUNT;
    cp_init.start_on_notify_cccd_handle    = BLE_GATT_HANDLE_INVALID;
    cp_init.disconnect_on_fail             = false;
    cp_init.evt_handler                    = on_conn_params_evt;
    cp_init.error_handler                  = conn_params_error_handler;

    err_code = ble_conn_params_init(&cp_init);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for putting the chip into sleep mode.
 *
 * @note This function will not return.
 */
static void sleep_mode_enter(void)
{
    uint32_t err_code = bsp_indication_set(BSP_INDICATE_IDLE);
    APP_ERROR_CHECK(err_code);

    // Prepare wakeup buttons.
    err_code = bsp_btn_ble_sleep_mode_prepare();
    APP_ERROR_CHECK(err_code);

    // Go to system-off mode (this function will not return; wakeup will cause a reset).
    err_code = sd_power_system_off();
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for handling advertising events.
 *
 * @details This function will be called for advertising events which are passed to the application.
 *
 * @param[in] ble_adv_evt  Advertising event.
 */
static void on_adv_evt(ble_adv_evt_t ble_adv_evt)
{
    uint32_t err_code;

    switch (ble_adv_evt)
    {
        case BLE_ADV_EVT_FAST:
            err_code = bsp_indication_set(BSP_INDICATE_ADVERTISING);
            APP_ERROR_CHECK(err_code);
            break;
        case BLE_ADV_EVT_IDLE:
            sleep_mode_enter();
            break;
        default:
            break;
    }
}


/**@brief Function for handling BLE events.
 *
 * @param[in]   p_ble_evt   Bluetooth stack event.
 * @param[in]   p_context   Unused.
 */
static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context)
{
    uint32_t err_code;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_CONNECTED:
			ble_status = BLE_STATUS_CONNECTED;
			printf("AT+STATUS:%X\r\n", ble_status);
            NRF_LOG_INFO("Connected");
            err_code = bsp_indication_set(BSP_INDICATE_CONNECTED);
            APP_ERROR_CHECK(err_code);
            m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
            break;

        case BLE_GAP_EVT_DISCONNECTED:
			ble_status = BLE_STATUS_DISCONNECTED;
			printf("AT+STATUS:%X\r\n", ble_status);
            NRF_LOG_INFO("Disconnected");
            // LED indication will be changed when advertising starts.
            m_conn_handle = BLE_CONN_HANDLE_INVALID;
            break;

#ifndef S140
        case BLE_GAP_EVT_PHY_UPDATE_REQUEST:
        {
            NRF_LOG_DEBUG("PHY update request.");
            ble_gap_phys_t const phys =
            {
                .rx_phys = BLE_GAP_PHY_AUTO,
                .tx_phys = BLE_GAP_PHY_AUTO,
            };
            err_code = sd_ble_gap_phy_update(p_ble_evt->evt.gap_evt.conn_handle, &phys);
            APP_ERROR_CHECK(err_code);
        } break;
#endif

        case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
            // Pairing not supported
            err_code = sd_ble_gap_sec_params_reply(m_conn_handle, BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP, NULL, NULL);
            APP_ERROR_CHECK(err_code);
            break;
#if !defined (S112)
         case BLE_GAP_EVT_DATA_LENGTH_UPDATE_REQUEST:
        {
            ble_gap_data_length_params_t dl_params;

            // Clearing the struct will effectivly set members to @ref BLE_GAP_DATA_LENGTH_AUTO
            memset(&dl_params, 0, sizeof(ble_gap_data_length_params_t));
            err_code = sd_ble_gap_data_length_update(p_ble_evt->evt.gap_evt.conn_handle, &dl_params, NULL);
            APP_ERROR_CHECK(err_code);
        } break;
#endif //!defined (S112)
        case BLE_GATTS_EVT_SYS_ATTR_MISSING:
            // No system attributes have been stored.
            err_code = sd_ble_gatts_sys_attr_set(m_conn_handle, NULL, 0, 0);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GATTC_EVT_TIMEOUT:
            // Disconnect on GATT Client timeout event.
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gattc_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GATTS_EVT_TIMEOUT:
            // Disconnect on GATT Server timeout event.
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gatts_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_EVT_USER_MEM_REQUEST:
            err_code = sd_ble_user_mem_reply(p_ble_evt->evt.gattc_evt.conn_handle, NULL);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST:
        {
            ble_gatts_evt_rw_authorize_request_t  req;
            ble_gatts_rw_authorize_reply_params_t auth_reply;

            req = p_ble_evt->evt.gatts_evt.params.authorize_request;

            if (req.type != BLE_GATTS_AUTHORIZE_TYPE_INVALID)
            {
                if ((req.request.write.op == BLE_GATTS_OP_PREP_WRITE_REQ)     ||
                    (req.request.write.op == BLE_GATTS_OP_EXEC_WRITE_REQ_NOW) ||
                    (req.request.write.op == BLE_GATTS_OP_EXEC_WRITE_REQ_CANCEL))
                {
                    if (req.type == BLE_GATTS_AUTHORIZE_TYPE_WRITE)
                    {
                        auth_reply.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
                    }
                    else
                    {
                        auth_reply.type = BLE_GATTS_AUTHORIZE_TYPE_READ;
                    }
                    auth_reply.params.write.gatt_status = APP_FEATURE_NOT_SUPPORTED;
                    err_code = sd_ble_gatts_rw_authorize_reply(p_ble_evt->evt.gatts_evt.conn_handle,
                                                               &auth_reply);
                    APP_ERROR_CHECK(err_code);
                }
            }
        } break; // BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST
		
		case BLE_GATTS_EVT_HVN_TX_COMPLETE:	
			// Send unfished uart received data to BLE
			err_code = ble_send_more_data();
			NRF_LOG_INFO("second_send,err_code:%x", err_code);
			APP_ERROR_CHECK(err_code);
			break;		

        default:
            // No implementation needed.
            break;
    }
}


/**@brief Function for the SoftDevice initialization.
 *
 * @details This function initializes the SoftDevice and the BLE event interrupt.
 */
static void ble_stack_init(void)
{
    ret_code_t err_code;

    err_code = nrf_sdh_enable_request();
    APP_ERROR_CHECK(err_code);

    // Configure the BLE stack using the default settings.
    // Fetch the start address of the application RAM.
    uint32_t ram_start = 0;
    err_code = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start);
    APP_ERROR_CHECK(err_code);

    // Enable BLE stack.
    err_code = nrf_sdh_ble_enable(&ram_start);
    APP_ERROR_CHECK(err_code);

    // Register a handler for BLE events.
    NRF_SDH_BLE_OBSERVER(m_ble_observer, APP_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);
}


/**@brief Function for handling events from the GATT library. */
void gatt_evt_handler(nrf_ble_gatt_t * p_gatt, nrf_ble_gatt_evt_t const * p_evt)
{
    if ((m_conn_handle == p_evt->conn_handle) && (p_evt->evt_id == NRF_BLE_GATT_EVT_ATT_MTU_UPDATED))
    {
        m_ble_nus_max_data_len = p_evt->params.att_mtu_effective - OPCODE_LENGTH - HANDLE_LENGTH;
        NRF_LOG_INFO("Data len is set to 0x%X(%d)", m_ble_nus_max_data_len, m_ble_nus_max_data_len);
    }
    NRF_LOG_DEBUG("ATT MTU exchange completed. central 0x%x peripheral 0x%x",
                  p_gatt->att_mtu_desired_central,
                  p_gatt->att_mtu_desired_periph);
}


/**@brief Function for initializing the GATT library. */
void gatt_init(void)
{
    ret_code_t err_code;

    err_code = nrf_ble_gatt_init(&m_gatt, gatt_evt_handler);
    APP_ERROR_CHECK(err_code);

    err_code = nrf_ble_gatt_att_mtu_periph_set(&m_gatt, 64);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for handling events from the BSP module.
 *
 * @param[in]   event   Event generated by button press.
 */
void bsp_event_handler(bsp_event_t event)
{
    uint32_t err_code;
    switch (event)
    {
        case BSP_EVENT_SLEEP:
            sleep_mode_enter();
            break;

        case BSP_EVENT_DISCONNECT:
            err_code = sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            if (err_code != NRF_ERROR_INVALID_STATE)
            {
                APP_ERROR_CHECK(err_code);
            }
            break;

        case BSP_EVENT_WHITELIST_OFF:
            if (m_conn_handle == BLE_CONN_HANDLE_INVALID)
            {
                err_code = ble_advertising_restart_without_whitelist(&m_advertising);
                if (err_code != NRF_ERROR_INVALID_STATE)
                {
                    APP_ERROR_CHECK(err_code);
                }
            }
            break;

        default:
            break;
    }
}


/**@brief   Function for handling app_uart events.
 *
 * @details This function will receive a single character from the app_uart module and append it to
 *          a string. The string will be be sent over BLE when the last character received was a
 *          'new line' '\n' (hex 0x0A) or if the string has reached the maximum data length.
 */
/**@snippet [Handling the data received over UART] */
void uart_event_handler(app_uart_evt_t * p_event)
{
    switch (p_event->evt_type)
    {
        case APP_UART_DATA_READY:
			nrf_drv_timer_disable(&TIMER_UART_RX);
			
			UNUSED_VARIABLE(app_uart_get(&UART_RX_BUF[UART_RX_STA]));
			UART_RX_STA++;	// Record the uart received data frame length
			
			nrf_drv_timer_enable(&TIMER_UART_RX);
            break;

        case APP_UART_COMMUNICATION_ERROR:
            APP_ERROR_HANDLER(p_event->data.error_communication);
            break;

        case APP_UART_FIFO_ERROR:
            APP_ERROR_HANDLER(p_event->data.error_code);
            break;

        default:
            break;
    }
}
/**@snippet [Handling the data received over UART] */


/**@brief  Function for initializing the UART module.
 *
 * @param[in] baudrate   uart baudrate 
 *
 * @return NRF_SUCCESS on success, otherwise an error code.  
 */
/**@snippet [UART Initialization] */
static uint32_t uart_init(uint32_t baudrate)
{
    uint32_t err_code;
    app_uart_comm_params_t const comm_params =
    {
        .rx_pin_no    = RX_PIN_NUMBER,
        .tx_pin_no    = TX_PIN_NUMBER,
        .rts_pin_no   = RTS_PIN_NUMBER,
        .cts_pin_no   = CTS_PIN_NUMBER,
        .flow_control = APP_UART_FLOW_CONTROL_DISABLED,
        .use_parity   = false,
        .baud_rate    = baudrate
    };

    APP_UART_FIFO_INIT(&comm_params,
                       UART_RX_BUF_SIZE,
                       UART_TX_BUF_SIZE,
                       uart_event_handler,
                       APP_IRQ_PRIORITY_LOWEST,
                       err_code);
//    APP_ERROR_CHECK(err_code);
	return err_code;
}
/**@snippet [UART Initialization] */


/**@brief Function for initializing the Advertising functionality.
 */
static void advertising_init(void)
{
    uint32_t               err_code;
    ble_advertising_init_t init;

    memset(&init, 0, sizeof(init));

    init.advdata.name_type          = BLE_ADVDATA_FULL_NAME;
    init.advdata.include_appearance = false;
    init.advdata.flags              = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;  // LE General Discoverable Mode, BR/EDR not supported.

    init.srdata.uuids_complete.uuid_cnt = sizeof(m_adv_uuids) / sizeof(m_adv_uuids[0]);
    init.srdata.uuids_complete.p_uuids  = m_adv_uuids;

    init.config.ble_adv_fast_enabled  = true;
    init.config.ble_adv_fast_interval = adv_interval;
    init.config.ble_adv_fast_timeout  = APP_ADV_TIMEOUT_IN_SECONDS;

    init.evt_handler = on_adv_evt;

    err_code = ble_advertising_init(&m_advertising, &init);
    APP_ERROR_CHECK(err_code);

    ble_advertising_conn_cfg_tag_set(&m_advertising, APP_BLE_CONN_CFG_TAG);
}


/**@brief Function for initializing buttons and leds.
 *
 * @param[out] p_erase_bonds  Will be true if the clear bonding button was pressed to wake the application up.
 */
static void buttons_leds_init(bool * p_erase_bonds)
{
    bsp_event_t startup_event;

    uint32_t err_code = bsp_init(BSP_INIT_LED | BSP_INIT_BUTTONS, bsp_event_handler);
    APP_ERROR_CHECK(err_code);

    err_code = bsp_btn_ble_init(NULL, &startup_event);
    APP_ERROR_CHECK(err_code);

    *p_erase_bonds = (startup_event == BSP_EVENT_CLEAR_BONDING_DATA);
}


/**@brief Function for initializing the nrf log module.
 */
static void log_init(void)
{
    ret_code_t err_code = NRF_LOG_INIT(NULL);
    APP_ERROR_CHECK(err_code);

    NRF_LOG_DEFAULT_BACKENDS_INIT();
}


/**@brief Function for placing the application in low power state while waiting for events.
 */
static void power_manage(void)
{
    uint32_t err_code = sd_app_evt_wait();
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for handling uart rx timeout.
 *
 * @param[in] event_type    Timer event type. 
 * @param[in] p_context     Unused.
 */
void timer_uart_rx_timeout_event_handler(nrf_timer_event_t event_type, void* p_context)
{
	uint32_t err_code;
	
	nrf_drv_timer_disable(&TIMER_UART_RX);
	
    switch (event_type)
    {
        case NRF_TIMER_EVENT_COMPARE0:
			if(AT_cmd_check_valid(UART_RX_BUF, UART_RX_STA))  // AT command
			{
				AT_cmd_handle(UART_RX_BUF, UART_RX_STA);
			}
			else  // Pass-through data
			{
				err_code = ble_send_data(UART_RX_BUF, UART_RX_STA);
				NRF_LOG_INFO("first_send,err_code:%x", err_code);
				APP_ERROR_CHECK(err_code);
			}

			
			UART_RX_STA=0;
            break;

        default:
            //Do nothing.
            break;
    }
}


/**@brief Function for initializing hardware timer.
 */
void timer_uart_rx_timeout_init(void)
{
    uint32_t time_ticks;
    ret_code_t err_code;

	// Define and init timer_cfg struct
	nrf_drv_timer_config_t timer_cfg = NRF_DRV_TIMER_DEFAULT_CONFIG;
	
	// Init timer
	err_code = nrf_drv_timer_init(	&TIMER_UART_RX, 
									&timer_cfg, 
									timer_uart_rx_timeout_event_handler);// Register callback	
	APP_ERROR_CHECK(err_code);

	// Convert alarm time(ms) to ticks 
	time_ticks = nrf_drv_timer_ms_to_ticks(&TIMER_UART_RX, UART_RX_TIMEOUT_INTERVAL);
	
	// Setup timer channel
	nrf_drv_timer_extended_compare(	&TIMER_UART_RX, 
									NRF_TIMER_CC_CHANNEL0,	// Timer channel 
									time_ticks, 
									NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK, 
									true);	
}

/**@brief Function for the Timer initialization.
 *
 * @details Initializes the timer module. This initial and creates application timers.
 */
static void timers_init(void)
{
    ret_code_t err_code;

	err_code = app_timer_init();
    APP_ERROR_CHECK(err_code);

    err_code = app_timer_create(&wdt_feed_timer_id, 
								APP_TIMER_MODE_REPEATED, 
								wdt_feed_timer_handler);
    APP_ERROR_CHECK(err_code);		
}


/**@brief Function for initializing the wdt module. */
void wdt_init(void)
{
	// Configure WDT.
	uint32_t err_code = NRF_SUCCESS;
	
    nrf_drv_wdt_config_t config = NRF_DRV_WDT_DEAFULT_CONFIG;
    err_code = nrf_drv_wdt_init(&config, wdt_event_handler);
    APP_ERROR_CHECK(err_code);
	
    err_code = nrf_drv_wdt_channel_alloc(&m_channel_id);
    APP_ERROR_CHECK(err_code);
	
    nrf_drv_wdt_enable();
}


/**@brief Application main function.
 */
int main(void)
{
    uint32_t err_code;
    bool     erase_bonds;

    // Initialize.
    timers_init();
	
	timer_uart_rx_timeout_init();

    err_code = uart_init(UART_BAUDRATE_BAUDRATE_Baud115200);
	APP_ERROR_CHECK(err_code);
	
    log_init();

    buttons_leds_init(&erase_bonds);
    ble_stack_init();
    gap_params_init();
    gatt_init();
    services_init();
    advertising_init();
    conn_params_init();
	
	 wdt_init();
	 wdt_feed_timers_start();

    printf("\r\nAT+RESET:OK\r\n");
    NRF_LOG_INFO("BLE_UART Start!");
	
    err_code = ble_advertising_start(&m_advertising, BLE_ADV_MODE_FAST);
    APP_ERROR_CHECK(err_code);

    // Enter main loop.
    for (;;)
    {
//        UNUSED_RETURN_VALUE(NRF_LOG_PROCESS());
		
        if (NRF_LOG_PROCESS() == false)
        {
            power_manage();
        }
    }
}


/**
 * @}
 */
