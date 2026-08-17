#include <ch32x035.h> /* both X033 and X035 */
#include <stdlib.h>   /* atoi() */
#include <string.h>   /* memset() */

#include <wch_usbhid_internal.h>

#include "keycodes.h"
/* we use our own custom debug lib
 * because the framework-wch-noneos-sdk does not allow disabling printf()
 */
#include "debug.h"

/* I2C on the expansion connector towards the badge */
#define SDA_PORT         GPIOC
#define SDA_PIN          GPIO_Pin_18
#define SCL_PORT         GPIOC
#define SCL_PIN          GPIO_Pin_19
#define I2C_ADDRESS      (0x39)
#define I2C_TIMEOUT      (-2)
#define I2C_SPEED        (400000)
#define UART_BAUDRATE    (115200)

/* digital outputs (button matrix) */
#define COL0_PORT GPIOA // PA0: col0
#define COL0_PIN  GPIO_Pin_0
#define COL1_PORT GPIOA // PA1: col1
#define COL1_PIN  GPIO_Pin_1
#define COL2_PORT GPIOA // PA2: col2
#define COL2_PIN  GPIO_Pin_2
#define COL3_PORT GPIOA // PA3: col3
#define COL3_PIN  GPIO_Pin_3
#define COL4_PORT GPIOA // PA4: col4
#define COL4_PIN  GPIO_Pin_4
#define COL5_PORT GPIOA // PA5: col5
#define COL5_PIN  GPIO_Pin_5
#define COL6_PORT GPIOA // PA6: col6
#define COL6_PIN  GPIO_Pin_6
#define COL7_PORT GPIOA // PA7: col7
#define COL7_PIN  GPIO_Pin_7
#define COL8_PORT GPIOB // PB4: col8
#define COL8_PIN  GPIO_Pin_4
#define N_COLS    (9)

#define ROW0_PORT GPIOB // PB0: row0
#define ROW0_PIN  GPIO_Pin_0
#define ROW1_PORT GPIOB // PB3: row1
#define ROW1_PIN  GPIO_Pin_3
#define ROW2_PORT GPIOB // PB5: row2 (TODO: PB1?)
#define ROW2_PIN  GPIO_Pin_5
#define ROW3_PORT GPIOB // PB6: row3
#define ROW3_PIN  GPIO_Pin_6
#define ROW4_PORT GPIOB // PB7: row4
#define ROW4_PIN  GPIO_Pin_7
#define ROW5_PORT GPIOB // PB8: row5
#define ROW5_PIN  GPIO_Pin_8
#define ROW6_PORT GPIOB // PB9: row6
#define ROW6_PIN  GPIO_Pin_9
#define ROW7_PORT GPIOB // PB12: row7
#define ROW7_PIN  GPIO_Pin_12
#define N_ROWS    (8) // fits perfectly in uint8_t

#define TIMER_FREQ ((SystemCoreClock / 10000) - 1) /* the output frequency of all timers: 100Hz */

/* backlight PWM */
#define BACKLIGHT_PORT            GPIOC // PC3: backlight LED
#define BACKLIGHT_PIN             GPIO_Pin_3
#define BACKLIGHT_TIM             TIM1                    // TIM1 Channel 4
#define BACKLIGHT_TIM_REMAP       GPIO_PartialRemap3_TIM1 // mapping 0b011
#define BACKLIGHT_TIM_CVR         TIM1->CH4CVR            // TIM1 Channel 4 compare register
#define BACKLIGHT_TIM_DMA_CHANNEL DMA1_Channel5           // DMA channel for TIM1_UP

/* UART */
#define UART_PORT   GPIOB
#define UART_TX_PIN GPIO_Pin_10 // PB10: UART TX
#define UART_RX_PIN GPIO_Pin_11 // PB11: UART RX
#define UART        USART1

/* 3 bytes: version number
 * 1 byte: button matrix state
 * 2 * ADC_CHANNELS bytes: 1 analog input is 16 bit
 * 1 byte: left encoder value
 * 1 byte: right encoder value
 * 3 * LEDS_NUM bytes: red, green, blue value for each LED
 * */
#define RESULT_BUFFER_SIZE      (3 + 8 + 1 + 2)
#define RESULT_KB_OFFSET        (3)
#define RESULT_CONFIG_OFFSET    (RESULT_KB_OFFSET + 8)
#define RESULT_BACKLIGHT_OFFSET (RESULT_CONFIG_OFFSET + 1)

#define HID_REPORT_KEYS (6)

typedef struct
{
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[HID_REPORT_KEYS];
} key_report_t;

/*
 * This struct contains all data that is available through I2C.
 * Use the following command with a Buspirate to test:
 * read version number : [ 0x72 0x00 [ 0x73 r:3 ]
 * read matrix state : [ 0x72 0x03 [ 0x73 r:1 ]
 * read analog inputs : [ 0x72 0x04 [ 0x73 r:16 ]
 * read buttons and analog inputs : [ 0x72 0x03 [ 0x73 r:17 ]
 * read left encoder : [ 0x72 0x14 [ 0x73 r:1 ]
 * read right encoder : [ 0x72 0x15 [ 0x73 r:1 ]
 * read both encoders : [ 0x72 0x14 [ 0x73 r:2 ]
 * turn on all leds : [ 0x72 0x16 0xFF:24 ]
 * turn off all leds : [ 0x72 0x16 0x00:24 ]
 * read everything : [ 0x72 0x00 [ 0x73 r:46 ]
 */
typedef struct __attribute__((packed))
{
    uint8_t version[3];      // version number
    key_report_t key_report; // reference to the button state byte in the result buffer
    uint8_t enable_int : 1;         // configuration flag to enable interrupt output instead of UART output (TODO)
    uint8_t reboot : 1;             // configuration flag to trigger a reboot to bootloader
    uint8_t remap : 1;              // write 1 to remap the SWD to the I2C pins
    uint8_t enable_uart_output : 1; // configuration flag to enable sending the HID report over UART; defaults to 1. When 0, UART is disabled and its pins are tri-stated
    uint8_t reserved : 4;           // reserved
    uint16_t backlight;      // backlight PWM value
} addon_data_t;

_Static_assert(sizeof(addon_data_t) == RESULT_BUFFER_SIZE, "raw data and struct size are not aligned!");

typedef struct
{
    uint8_t flag_matrix_scan_done : 1;    // flag to indicate that the button matrix state has changed
    uint8_t flag_int_should_clear : 1;    // flag to indicate that the interrupt should be cleared
    uint8_t flag_button_scan_halfway : 1; // flag to indicate that the matrix scan is halfway
    uint8_t flag_caps_lock : 1;           // reserved for future use
    uint8_t flag_config_changed : 1;      // flag to indicate that the configuration has changed through I2C
    uint8_t flag_slave_first_write : 1;   // set on every ADDR phase; the next RXNE byte is the register offset
    uint8_t flag_uart_enabled : 1;
    uint8_t reserved : 1;                 // reserved for future use
    uint8_t matrix_state[N_COLS];         // current matrix state
    uint8_t slave_offset;                 // register offset captured after the most recent ADDR+W
    uint8_t slave_position;              // current read/write cursor, reset to offset on every ADDR
    union
    {
        addon_data_t data;
        uint8_t raw_data[RESULT_BUFFER_SIZE];
    };
} addon_state_t;

/* Global Variables */
static addon_state_t state;

/* initialize timer 3 to periodically generate an interrupt */
static void TIM3_Init(uint16_t arr, uint16_t psc)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure = {0};
    NVIC_InitTypeDef NVIC_InitStructure = {0};

    /* Enable Timer3 Clock */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);

    /* Initialize Timer3 */
    TIM_TimeBaseStructure.TIM_Period = arr;
    TIM_TimeBaseStructure.TIM_Prescaler = psc;
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &TIM_TimeBaseStructure);

    /* enable timer interrupts */
    TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);

    /* configure timer interrupt */
    NVIC_InitStructure.NVIC_IRQChannel = TIM3_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    /* Enable Timer3 */
    TIM_Cmd(TIM3, ENABLE);
}

/* set a matrix column active */
static void KB_Set_Col(uint8_t col)
{
    switch (col)
    {
        case 0:
            GPIO_WriteBit(COL8_PORT, COL8_PIN, Bit_SET);
            GPIO_WriteBit(COL0_PORT, COL0_PIN, Bit_RESET);
            break;
        case 1:
            GPIO_WriteBit(COL0_PORT, COL0_PIN, Bit_SET);
            GPIO_WriteBit(COL1_PORT, COL1_PIN, Bit_RESET);
            break;
        case 2:
            GPIO_WriteBit(COL1_PORT, COL1_PIN, Bit_SET);
            GPIO_WriteBit(COL2_PORT, COL2_PIN, Bit_RESET);
            break;
        case 3:
            GPIO_WriteBit(COL2_PORT, COL2_PIN, Bit_SET);
            GPIO_WriteBit(COL3_PORT, COL3_PIN, Bit_RESET);
            break;
        case 4:
            GPIO_WriteBit(COL3_PORT, COL3_PIN, Bit_SET);
            GPIO_WriteBit(COL4_PORT, COL4_PIN, Bit_RESET);
            break;
        case 5:
            GPIO_WriteBit(COL4_PORT, COL4_PIN, Bit_SET);
            GPIO_WriteBit(COL5_PORT, COL5_PIN, Bit_RESET);
            break;
        case 6:
            GPIO_WriteBit(COL5_PORT, COL5_PIN, Bit_SET);
            GPIO_WriteBit(COL6_PORT, COL6_PIN, Bit_RESET);
            break;
        case 7:
            GPIO_WriteBit(COL6_PORT, COL6_PIN, Bit_SET);
            GPIO_WriteBit(COL7_PORT, COL7_PIN, Bit_RESET);
            break;
        case 8:
            GPIO_WriteBit(COL7_PORT, COL7_PIN, Bit_SET);
            GPIO_WriteBit(COL8_PORT, COL8_PIN, Bit_RESET);
            break;
        default:
            GPIO_WriteBit(GPIOA, COL0_PIN | COL1_PIN | COL2_PIN | COL3_PIN | COL4_PIN | COL5_PIN | COL6_PIN | COL7_PIN, Bit_SET);
            GPIO_WriteBit(COL8_PORT, COL8_PIN, Bit_SET);
    }
}

/* initialize the I2C interface */
static void IIC_Init(uint32_t bound, uint16_t address)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    I2C_InitTypeDef I2C_InitStructure = {0};
    NVIC_InitTypeDef NVIC_InitStruct = {0};

    /* enable I2C1 and GPIOC clocks */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C1, ENABLE);

    /* remap PC18/PC19 to I2C1 SDA/SCL */
    GPIO_PinRemapConfig(GPIO_PartialRemap3_I2C1, ENABLE); // 011: Mapping (SCL/PC19, SDA/PC18)

    /* Disable DIO (SWD) interface on these pins */
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_Disable, ENABLE);

    /* configure the GPIO as SDA/SCL pins */
    GPIO_InitStructure.GPIO_Pin = SDA_PIN | SCL_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP; // automatic open-drain
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    /* configure I2C1 */
    I2C_InitStructure.I2C_ClockSpeed = bound;                                 // bus speed
    I2C_InitStructure.I2C_Mode = I2C_Mode_I2C;                                // there is only 1 mode
    I2C_InitStructure.I2C_DutyCycle = I2C_DutyCycle_16_9;                     // I2C fast mode Tlow/Thigh = 16/9
    I2C_InitStructure.I2C_OwnAddress1 = address << 1;                         // 7 or 10 bit address
    I2C_InitStructure.I2C_Ack = I2C_Ack_Enable;                               // automatic acknowledge
    I2C_InitStructure.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit; // use 7 bit address
    I2C_Init(I2C1, &I2C_InitStructure);

    /* configure I2C interrupts */
    NVIC_InitStruct.NVIC_IRQChannel = I2C1_EV_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 1;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);

    NVIC_InitStruct.NVIC_IRQChannel = I2C1_ER_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 1;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStruct);

    /* Enable I2C event, error, and buffer interrupts.
     * EVT fires on: address match, byte received, byte transmitted, stop detected.
     * ERR fires on: bus error, arbitration lost, acknowledge failure, etc.
     * BUF fires on: TXE/RXNE (needed so we get an interrupt for each data byte).
     */
    I2C_ITConfig(I2C1, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF, ENABLE);

    /* enable clock stretching */
    I2C_StretchClockCmd(I2C1, ENABLE);

    /* enable I2C1 */
    I2C_Cmd(I2C1, ENABLE);
}

// reference: https://github.com/openwch/ch32x035/blob/main/EVT/EXAM/TIM/TIM_DMA/User/main.c
static void Backlight_PWM_Init(uint16_t arr, uint16_t psc, uint16_t ccp)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStructure = {0};
    TIM_OCInitTypeDef TIM_OCInitStructure = {0};

    /* enable timers and GPIO clocks */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO | RCC_APB2Periph_TIM1 | RCC_APB2Periph_GPIOC, ENABLE);

    /* set pinmux */
    GPIO_PinRemapConfig(BACKLIGHT_TIM_REMAP, ENABLE); // set LCD backlight (PC3) on CH4 of TIM1

    GPIO_InitStructure.GPIO_Pin = BACKLIGHT_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(BACKLIGHT_PORT, &GPIO_InitStructure);

    TIM_TimeBaseInitStructure.TIM_Period = arr;
    TIM_TimeBaseInitStructure.TIM_Prescaler = psc;
    TIM_TimeBaseInitStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInitStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInitStructure.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(BACKLIGHT_TIM, &TIM_TimeBaseInitStructure);

    TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM2; // High until CNT < CCR
    TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStructure.TIM_Pulse = ccp; // start duty cycle
    TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_Low;
    TIM_OC4Init(BACKLIGHT_TIM, &TIM_OCInitStructure);

    TIM_OC4PreloadConfig(BACKLIGHT_TIM, TIM_OCPreload_Disable);
    TIM_ARRPreloadConfig(BACKLIGHT_TIM, ENABLE);
}

static void Backlight_PWM_DMA_Init(u32 memadr)
{
    DMA_InitTypeDef DMA_InitStructure = {0};

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    DMA_DeInit(BACKLIGHT_TIM_DMA_CHANNEL);
    DMA_InitStructure.DMA_PeripheralBaseAddr = (u32)&BACKLIGHT_TIM_CVR;
    DMA_InitStructure.DMA_MemoryBaseAddr = memadr;
    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralDST;
    DMA_InitStructure.DMA_BufferSize = 1;
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Disable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;
    DMA_InitStructure.DMA_Priority = DMA_Priority_High;
    DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(BACKLIGHT_TIM_DMA_CHANNEL, &DMA_InitStructure);

    DMA_Cmd(BACKLIGHT_TIM_DMA_CHANNEL, ENABLE);
}

/* initialize all GPIOs related to the keyboard */
static void KB_Scan_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    /* Enable GPIOB clock */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    /* the columns are the outputs */
    GPIO_InitStructure.GPIO_Pin = COL0_PIN | COL1_PIN | COL2_PIN | COL3_PIN | COL4_PIN | COL5_PIN | COL6_PIN | COL7_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = COL8_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(COL8_PORT, &GPIO_InitStructure);

    /* the rows are the inputs */
    GPIO_InitStructure.GPIO_Pin = ROW0_PIN | ROW1_PIN | ROW2_PIN | ROW3_PIN | ROW4_PIN | ROW5_PIN | ROW6_PIN | ROW7_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    /* put all outputs high*/
    KB_Set_Col(99);

    /* put col0 to low */
    KB_Set_Col(0);
}

/* Configure keyboard wake up mode. */
// static void KB_Sleep_Wakeup_Cfg(void)
// {
//     EXTI_InitTypeDef EXTI_InitStructure = {0};

//     RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);

//     // TODO
//     GPIO_EXTILineConfig(GPIO_PortSourceGPIOD, GPIO_PinSource1);
//     EXTI_InitStructure.EXTI_Line = EXTI_Line1;
//     EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Event;
//     EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising; // TODO: falling?
//     EXTI_InitStructure.EXTI_LineCmd = ENABLE;
//     EXTI_Init(&EXTI_InitStructure);

//     GPIO_EXTILineConfig(GPIO_PortSourceGPIOA, GPIO_PinSource3);
//     EXTI_InitStructure.EXTI_Line = EXTI_Line3;
//     EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Event;
//     EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
//     EXTI_InitStructure.EXTI_LineCmd = ENABLE;
//     EXTI_Init(&EXTI_InitStructure);

//     GPIO_EXTILineConfig(GPIO_PortSourceGPIOA, GPIO_PinSource4);
//     EXTI_InitStructure.EXTI_Line = EXTI_Line4;
//     EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Event;
//     EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
//     EXTI_InitStructure.EXTI_LineCmd = ENABLE;
//     EXTI_Init(&EXTI_InitStructure);

//     GPIO_EXTILineConfig(GPIO_PortSourceGPIOB, GPIO_PinSource5);
//     GPIO_EXTILineConfig(GPIO_PortSourceGPIOA, GPIO_PinSource5);
//     EXTI_InitStructure.EXTI_Line = EXTI_Line5;
//     EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Event;
//     EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
//     EXTI_InitStructure.EXTI_LineCmd = ENABLE;
//     EXTI_Init(&EXTI_InitStructure);

//     GPIO_EXTILineConfig(GPIO_PortSourceGPIOA, GPIO_PinSource6);
//     EXTI_InitStructure.EXTI_Line = EXTI_Line6;
//     EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Event;
//     EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
//     EXTI_InitStructure.EXTI_LineCmd = ENABLE;
//     EXTI_Init(&EXTI_InitStructure);

//     GPIO_EXTILineConfig(GPIO_PortSourceGPIOA, GPIO_PinSource7);
//     EXTI_InitStructure.EXTI_Line = EXTI_Line7;
//     EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Event;
//     EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
//     EXTI_InitStructure.EXTI_LineCmd = ENABLE;
//     EXTI_Init(&EXTI_InitStructure);

//     GPIO_EXTILineConfig(GPIO_PortSourceGPIOA, GPIO_PinSource9);
//     EXTI_InitStructure.EXTI_Line = EXTI_Line9;
//     EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Event;
//     EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
//     EXTI_InitStructure.EXTI_LineCmd = ENABLE;
//     EXTI_Init(&EXTI_InitStructure);

//     EXTI->INTENR |= EXTI_INTENR_MR1 | EXTI_INTENR_MR3 | EXTI_INTENR_MR4 | EXTI_INTENR_MR5 | EXTI_INTENR_MR6 | EXTI_INTENR_MR7 | EXTI_INTENR_MR9;
// }

/* get the activated rows when a certain column is active */
static uint8_t io_to_scan_result(uint16_t b)
{
    uint8_t out = 0;

    if (!(b & (ROW7_PIN))) // PB12: row7
    {
        out |= 1;
    }
    out <<= 1;

    if (!(b & (ROW6_PIN))) // PB9: row6
    {
        out |= 1;
    }
    out <<= 1;

    if (!(b & (ROW5_PIN))) // PB8: row5
    {
        out |= 1;
    }
    out <<= 1;

    if (!(b & (ROW4_PIN))) // PB7: row4
    {
        out |= 1;
    }
    out <<= 1;

    if (!(b & (ROW3_PIN))) // PB6: row3
    {
        out |= 1;
    }
    out <<= 1;

    if (!(b & (ROW2_PIN))) // PB5: row2
    {
        out |= 1;
    }
    out <<= 1;

    if (!(b & (ROW1_PIN))) // PB3: row1
    {
        out |= 1;
    }
    out <<= 1;

    if (!(b & (ROW0_PIN))) // PB0: row0
    {
        out |= 1;
    }
    return out;
}

/* Perform the keyboard scan. */
static void KB_Scan(void)
{
    static uint8_t scan_cnt = 0;
    static uint8_t scan_col = 0;
    static uint8_t scan_result[N_COLS] = {0x00};
    static uint8_t scan = 0;

    scan_cnt++;
    if ((scan_cnt % 10) == 0) // every 100 ms
    {
        scan_cnt = 0;

        /* Determine whether the two scan results for this column are consistent (debouncing) */
        if (scan == io_to_scan_result(GPIO_ReadInputData(GPIOB)))
        {
            scan_result[scan_col] = scan;
        }

        /* activate the next column */
        scan_col = (scan_col + 1) % N_COLS;
        KB_Set_Col(scan_col);

        /* copy the full scan result to the global state */
        if (scan_col == 0)
        {
            memcpy(state.matrix_state, scan_result, N_COLS);
            state.flag_matrix_scan_done = 1; // indicate that a full scan was finished
            memset(scan_result, 0, N_COLS);
        }
    }
    else if ((scan_cnt % 5) == 0) // every 50 ms
    {
        /* Save the first scan result */
        scan = io_to_scan_result(GPIO_ReadInputData(GPIOB));
        state.flag_int_should_clear = 1;
    }
}

/* get the keycode of a keyboard key at row and column */
static uint8_t get_keycode(uint8_t col, uint8_t row, uint8_t *keycode)
{
    *keycode = modifiers[col][row];

    if (*keycode != KEY_NONE)
    {
        return 1;
    }

    *keycode = keycodes[col][row];
    if (*keycode != KEY_NONE)
    {
        return 0;
    }

    PRINT("ERROR: no keycode for row %d col %d\r\n", row, col);
    return 2; // error
}

/* update the key report with a new key press/release */
static void determine_hid_report(uint8_t keycode, uint8_t is_modifier, uint8_t pressed, key_report_t *out)
{
    uint8_t j;
    static uint8_t key_cnt = 0;

    if (is_modifier)
    {
        if (pressed)
        {
            out->modifiers |= keycode;
        }
        else
        {
            out->modifiers &= (~keycode);
        }
        return;
    }

    if (pressed)
    {
        if (key_cnt < HID_REPORT_KEYS)
        {
            out->keys[key_cnt++] = keycode;
        }
        else
        {
            PRINT("too many keys at the same time to report\r\n");
        }
    }
    else
    {
        for (j = 0; j < HID_REPORT_KEYS; j++)
        {
            if (out->keys[j] == keycode)
            {
                /* key found in the report */
                break;
            }
        }
        /* remove the key from the report */
        if (j == HID_REPORT_KEYS)
        {
            PRINT("released key not found in the report, removing last one\r\n");
        }
        else
        {
            memcpy(&out->keys[j], &out->keys[j + 1], (HID_REPORT_KEYS - j - 1));
        }
        /* clear the last key report entry */
        out->keys[5] = 0;
        if (key_cnt > 0)
        {
            key_cnt--;
        }
    }
}

/* toggle the backlight on or off */
static void toggle_backlight(void)
{
    state.data.backlight -= state.data.backlight % 20;
    state.data.backlight = (state.data.backlight + 20) % 120;
}

static void handle_fn(key_report_t *in, key_report_t *out)
{
    uint8_t j;

    /* caps lock is enabled by pressing the FN key and right shift at the same time */
    if ((in->modifiers & (KEY_MOD_RMETA | KEY_MOD_RSHIFT)) == (KEY_MOD_RMETA | KEY_MOD_RSHIFT))
    {
        determine_hid_report(KEY_CAPSLOCK, 0, 1, in);
        state.flag_caps_lock = 1;
    }
    else
    {
        if (state.flag_caps_lock)
        {
            determine_hid_report(KEY_CAPSLOCK, 0, 0, in);
            state.flag_caps_lock = 0;
        }
    }

    /* take a copy of the input report */
    memcpy(out, in, sizeof(key_report_t));

    /*
      if the FN key is pressed, modify the output,
      and trigger special functions
    */
    if (out->modifiers & KEY_MOD_RMETA)
    {
        /* replace keys and/or trigger special functions */
        for (j = 0; j < HID_REPORT_KEYS; j++)
        {
            switch (out->keys[j])
            {
                case KEY_SPACE:
                    toggle_backlight();
                    memcpy(&out->keys[j], &out->keys[j + 1], (HID_REPORT_KEYS - j - 1));
                    out->keys[5] = 0;
                    break;
                case KEY_BACKSPACE:
                    out->keys[j] = KEY_DELETE;
                    break;
                case KEY_LEFT:
                    out->keys[j] = KEY_HOME;
                    break;
                case KEY_RIGHT:
                    out->keys[j] = KEY_END;
                    break;
                case KEY_UP:
                    out->keys[j] = KEY_PAGEUP;
                    break;
                case KEY_DOWN:
                    out->keys[j] = KEY_PAGEDOWN;
                    break;
                default:
                    break;
            }
        }

        /* clear the FN key in the reporting */
        out->modifiers &= (~KEY_MOD_RMETA);
    }
}

/* configure UART1 as output */
static void USART_Output_Init(uint32_t baudrate)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1 | RCC_APB2Periph_GPIOB, ENABLE);

    GPIO_InitStructure.GPIO_Pin = UART_TX_PIN | UART_RX_PIN;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(UART_PORT, &GPIO_InitStructure);

    USART_InitStructure.USART_BaudRate = baudrate;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;

    USART_Init(UART, &USART_InitStructure);
    USART_Cmd(UART, ENABLE);
    state.flag_uart_enabled = 1;
}

/* disable UART1 and tri-state its pins so they do not interfere with signaling */
static void USART_Output_DeInit(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    state.flag_uart_enabled = 0;
    USART_Cmd(UART, DISABLE);

    GPIO_InitStructure.GPIO_Pin = UART_TX_PIN | UART_RX_PIN;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(UART_PORT, &GPIO_InitStructure);
}

static int i2c_pos_is_writable(uint8_t pos)
{
    if (pos >= RESULT_CONFIG_OFFSET && pos < RESULT_CONFIG_OFFSET + 3) return 1;
    return 0;
}

/**
 * Handle one I2C event interrupt for the slave register interface.
 *
 * Protocol (write transaction):
 *   START | ADDR+W | reg_offset | [data bytes...] | STOP
 *
 * Protocol (read transaction — master sets register first, then re-reads):
 *   START | ADDR+W | reg_offset | START | ADDR+R | [data bytes from reg_offset onwards...] | STOP
 *
 * The first byte of every write sets state.slave_position (the register address).
 * Subsequent bytes are written to raw_data[] if the position is writable.
 * Writing to RESULT_CONFIG_OFFSET raises flag_config_changed for the main loop.
 *
 * On TXE (master is reading): bytes are sent sequentially from raw_data[] starting at
 * state.slave_position, advancing the pointer with each byte sent.
 *
 * Reading STAR2 clears the ADDR flag as a hardware side-effect.
 * This releases the clock when clock stretching is enabled.
 */
static void i2c_slave_process(void)
{
    uint32_t flag1 = 0, flag2 = 0;
    flag1 = I2C1->STAR1;

    if (flag1 & I2C_STAR1_ADDR)
    {
        state.slave_position = state.slave_offset;
        state.flag_slave_first_write = 1;
    }

    if (flag1 & I2C_STAR1_RXNE)
    {
        uint8_t byte = I2C_ReceiveData(I2C1);
        if (state.flag_slave_first_write)
        {
            state.slave_offset = byte;
            state.slave_position = byte;
            state.flag_slave_first_write = 0;
            PRINT("I2C reg: 0x%02x\r\n", byte);
        }
        else
        {
            if (i2c_pos_is_writable(state.slave_position))
            {
                state.raw_data[state.slave_position] = byte;
                if (state.slave_position == RESULT_CONFIG_OFFSET)
                {
                    state.flag_config_changed = 1;
                }
            }
            state.slave_position++;
        }
    }

    if (flag1 & I2C_STAR1_TXE)
    {
        if (state.slave_position < RESULT_BUFFER_SIZE)
        {
            I2C_SendData(I2C1, state.raw_data[state.slave_position++]);
        }
        else
        {
            I2C_SendData(I2C1, 0x00);
        }
    }

    if (flag1 & I2C_STAR1_STOPF)
    {
        PRINT("I2C STOP\r\n");
        /* writing CTLR1 after reading STAR1 clears STOPF */
        I2C1->CTLR1 &= ~(I2C_CTLR1_STOP);

        /* Re-arm "next byte is a register offset" here too, not just on ADDR.
         * If back-to-back transactions leave too little bus-free time, the next
         * transaction's ADDR event can be missed/coalesced; without this, its
         * offset byte would be written into raw_data[] as stray data instead of
         * being captured as the new offset.
         */
        state.flag_slave_first_write = 1;
    }

    /* clock stretching: release the clock */
    flag2 = I2C1->STAR2;
    (void)flag2;
}

/* 2 breath pulses of the backlight */
static void boot_animation(void)
{
    for (uint16_t i = 0; i < 100; i++)
    {
        state.data.backlight = i;
        Delay_Ms(3);
    }
    for (uint16_t i = 100; i > 0; i--)
    {
        state.data.backlight = i;
        Delay_Ms(3);
    }
    for (uint16_t i = 0; i < 100; i++)
    {
        state.data.backlight = i;
        Delay_Ms(3);
    }
    for (uint16_t i = 100; i > 0; i--)
    {
        state.data.backlight = i;
        Delay_Ms(3);
    }
}

/* Main program */
int main(void)
{
    uint8_t previous_matrix_state[N_COLS];
    key_report_t key_report;
    uint8_t previous_c = 200;

    memset(previous_matrix_state, 0, N_COLS);
    memset(&key_report, 0, sizeof(key_report_t));

    /* set all data and flags to 0 */
    memset(&state, 0, sizeof(addon_state_t));

    /* set the version number from git */
    char version_major[] = VERSION_MAJOR;
    char version_minor[] = VERSION_MINOR;
    char version_patch[] = VERSION_PATCH;
    state.data.version[0] = atoi(version_major) & 0xff;
    state.data.version[1] = atoi(version_minor) & 0xff;
    state.data.version[2] = atoi(version_patch) & 0xff;

    SystemInit();
#ifdef NVIC_PriorityGroup_2
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
#else
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
#endif
    SystemCoreClockUpdate();
    Delay_Init();

    /* makes sure that we can still flash using SWD */
    Delay_Ms(1000);

    /* initialize i2c */
    IIC_Init(I2C_SPEED, I2C_ADDRESS); // disables SWD

    PRINT("SystemClk: %u\r\n", (unsigned)SystemCoreClock);
    PRINT("ChipID: %08x\r\n", (unsigned)DBGMCU_GetCHIPID());

    /* configure the keyboard backlight PWM output using DMA */
    Backlight_PWM_Init(100, TIMER_FREQ, state.data.backlight);
    Backlight_PWM_DMA_Init((u32)&state.data.backlight);
    TIM_DMACmd(BACKLIGHT_TIM, TIM_DMA_Update, ENABLE);
    TIM_Cmd(BACKLIGHT_TIM, ENABLE);
    TIM_CtrlPWMOutputs(BACKLIGHT_TIM, ENABLE);

    /* Initialize GPIO for keyboard scan */
    KB_Scan_Init();
    // KB_Sleep_Wakeup_Cfg();

    /* Initialize timer for Keyboard and mouse scan timing */
    TIM3_Init(1, TIMER_FREQ); // every 10 ms

    /* Initialize USBFS interface to communicate with the host  */
    USB_init();

    boot_animation();

    /* set the keyboard backlight off */
    state.data.backlight = 0;

    /* UART output is enabled by default; can be disabled through I2C configuration */
    state.data.enable_uart_output = 1;
    state.flag_uart_enabled = 0;
    state.flag_config_changed = 1;

    while (1)
    {
        if (state.flag_button_scan_halfway && state.data.enable_int)
        {
            state.flag_button_scan_halfway = 0;
            // TODO: set interrupt pin low
        }

        if (state.flag_matrix_scan_done)
        {
            state.flag_matrix_scan_done = 0;

            if (memcmp(state.matrix_state, previous_matrix_state, N_COLS) != 0)
            {
                // matrix state has changed
                for (int c = 0; c < N_COLS; c++)
                {
                    if (state.matrix_state[c] != previous_matrix_state[c])
                    {
                        for (int r = 0; r < N_ROWS; r++)
                        {
                            uint8_t current_button_state = (state.matrix_state[c] & (1 << r)) & 0xff;
                            uint8_t previous_button_state = (previous_matrix_state[c] & (1 << r)) & 0xff;

                            if (current_button_state != previous_button_state)
                            {
                                uint8_t keycode;
                                uint8_t is_modifier = get_keycode(c, r, &keycode);
                                if (is_modifier != 2)
                                {
                                    determine_hid_report(keycode, is_modifier, current_button_state, &key_report);
                                }
                            }
                        }
                    }
                }

                /* Copy the keyboard data to the buffer of endpoint 1 and set the data uploading flag */
                memcpy(previous_matrix_state, state.matrix_state, N_COLS);

                /* handle special FN triggers */
                handle_fn(&key_report, &state.data.key_report);

                /* the key report is ready to be fetched through I2C, so let's set the interrupt */
                if (state.data.enable_int)
                {
                    // TODO: set the UART TX to high
                    PRINT("TODO: set the output pin\r\n");
                }
                else if (state.flag_uart_enabled)
                {
#if (DEBUG)
                    PRINT("Reporting HID through UART:\r\n");
                    for (int i = 0; i < sizeof(key_report_t); i++)
                    {
                        PRINT("0x%02x ", state.raw_data[RESULT_KB_OFFSET + i]);
                    }
                    PRINT("\r\n");
#else
                    for (int i = 0; i < sizeof(key_report_t); i++)
                    {
                        while (USART_GetFlagStatus(UART, USART_FLAG_TC) == RESET)
                            ;
                        USART_SendData(UART, state.raw_data[RESULT_KB_OFFSET + i]);
                    }
#endif
                }

                USB_write(&state.raw_data[RESULT_KB_OFFSET], sizeof(key_report_t));
            }
        }

        /* uart receive a backlight brightness */
        if (USART_GetFlagStatus(UART, USART_FLAG_RXNE) != RESET)
        {
            uint8_t c = USART_ReceiveData(UART);
            /* if we previously received a valid value, do a checksum check */
            if (previous_c <= 100 && (c == (previous_c ^ 0xFF)))
            {
                PRINT("setting backlight through UART: %d\r\n", previous_c);
                state.data.backlight = (uint16_t)previous_c;
                previous_c = 200; /* set the previous value to an invalid value */
            }
            else
            {
                previous_c = c;
            }
        }

        /* I2C master wrote a new value to the outputs register: apply it now.
         * Also handles the reboot-to-bootloader command if that bit is set.
         */
        if (state.flag_config_changed)
        {
            state.flag_config_changed = 0;
            if (state.data.reboot)
            {
                PRINT("Reboot to bootloader trigger\r\n");
                Delay_Ms(100);
                SystemReset_StartMode(Start_Mode_BOOT);
                NVIC_SystemReset();
            }
            if (state.data.remap)
            {
                PRINT("Remap SWD trigger\r\n");
                Delay_Ms(100);
                /* disable I2C interrupts */
                I2C_ITConfig(I2C1, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF, DISABLE);

                /* disable I2C1 */
                I2C_Cmd(I2C1, DISABLE);

                /* Re-enable DIO (SWD) interface on these pins */
                GPIO_PinRemapConfig(GPIO_Remap_SWJ_Disable, DISABLE);
            }
            if (state.data.enable_uart_output)
            {
                USART_Output_Init(UART_BAUDRATE);
            }
            else
            {
                USART_Output_DeInit();
            }
        }
    }
}

/* interrupt handlers */
void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void NMI_Handler(void)
{
    PRINT("NMI_Handler\r\n");
}

void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void)
{
    PRINT("HARDFAULT\r\n");
    while (1)
    {
    }
}

void I2C1_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void I2C1_IRQHandler(void)
{
    PRINT("I2C1_IRQHandler\r\n");
}

// Interrupt Service Routine for I2C1 Event
void I2C1_EV_IRQHandler(void) __attribute__((interrupt));
void I2C1_EV_IRQHandler(void)
{
    i2c_slave_process();
}

/* I2C1 Error — clear whichever error flag fired so the bus is not blocked. */
void I2C1_ER_IRQHandler(void) __attribute__((interrupt));
void I2C1_ER_IRQHandler(void)
{
    uint16_t STAR1 = I2C1->STAR1;
    if (STAR1 & I2C_STAR1_BERR) I2C1->STAR1 &= ~I2C_STAR1_BERR;
    if (STAR1 & I2C_STAR1_ARLO) I2C1->STAR1 &= ~I2C_STAR1_ARLO;
    if (STAR1 & I2C_STAR1_AF) I2C1->STAR1 &= ~I2C_STAR1_AF;
}

/*********************************************************************
 * @fn      TIM3_IRQHandler
 *
 * @brief   This function handles TIM3 global interrupt request.
 *
 * @return  none
 */
void TIM3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void TIM3_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM3, TIM_IT_Update) != RESET)
    {
        /* Clear interrupt flag */
        TIM_ClearITPendingBit(TIM3, TIM_IT_Update);

        /* Handle keyboard scan */
        KB_Scan();
    }
}
