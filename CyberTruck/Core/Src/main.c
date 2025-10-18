/* ===== STM32F103C8 (Blue Pill) — 4 Encoders + MPU9250 → UART (CMSIS + Register) =====
   Packet @ 100 Hz (binary, little-endian):
   Header: 0xAA 0x55
   Payload (packed, 46 bytes):
     uint32  t_ms;
     int32   ticksFL, ticksFR, ticksBL, ticksBR;
     float   ax, ay, az;        // m/s^2
     float   gx, gy, gz;        // rad/s
     uint16  flags;
   Checksum: 1 byte XOR of payload bytes
   Total bytes/frame = 2 + 46 + 1 = 49
*/

#include "main.h"
#include <stdint.h>
#include <string.h>

/*** PIN DEFINITIONS ***/
// Front-Left encoder: PA8, PA9
#define ENC_FL_PORT     GPIOA
#define ENC_FLA_PIN     8
#define ENC_FLB_PIN     9

// Back-Left encoder: PA10, PA11
#define ENC_BL_PORT     GPIOA
#define ENC_BLA_PIN     10
#define ENC_BLB_PIN     11

// Front-Right encoder: PA15, PB3 (JTAG pins)
#define ENC_FRA_PORT    GPIOA
#define ENC_FRA_PIN     15
#define ENC_FRB_PORT    GPIOB
#define ENC_FRB_PIN     3

// Back-Right encoder: PB4, PB5 (JTAG pins)
#define ENC_BR_PORT     GPIOB
#define ENC_BRA_PIN     4
#define ENC_BRB_PIN     5

// I2C1 for MPU9250: PB6=SCL, PB7=SDA
#define I2C_PORT        GPIOB
#define I2C_SCL_PIN     6
#define I2C_SDA_PIN     7

// USART3: PB10=TX, PB11=RX
#define UART_PORT       GPIOB
#define UART_TX_PIN     10
#define UART_RX_PIN     11

#define UART_BAUD       921600

/*** UTILITY MACROS ***/
#define BIT(n) (1U << (n))
#define READ_PIN(port, pin) (((port)->IDR & BIT(pin)) != 0)

/*** QUADRATURE DECODER ***/
volatile int32_t encFL = 0, encFR = 0, encBL = 0, encBR = 0;
volatile uint8_t prevFL = 0, prevFR = 0, prevBL = 0, prevBR = 0;

static const int8_t qLUT[16] = {
  0, -1, +1,  0,
  +1,  0,  0, -1,
  -1,  0,  0, +1,
   0, +1, -1,  0
};

static inline uint8_t rdFL(void){
  return ((READ_PIN(ENC_FL_PORT, ENC_FLA_PIN) ? 1 : 0) << 1) |
         (READ_PIN(ENC_FL_PORT, ENC_FLB_PIN) ? 1 : 0);
}
static inline uint8_t rdFR(void){
  return ((READ_PIN(ENC_FRA_PORT, ENC_FRA_PIN) ? 1 : 0) << 1) |
         (READ_PIN(ENC_FRB_PORT, ENC_FRB_PIN) ? 1 : 0);
}
static inline uint8_t rdBL(void){
  return ((READ_PIN(ENC_BL_PORT, ENC_BLA_PIN) ? 1 : 0) << 1) |
         (READ_PIN(ENC_BL_PORT, ENC_BLB_PIN) ? 1 : 0);
}
static inline uint8_t rdBR(void){
  return ((READ_PIN(ENC_BR_PORT, ENC_BRA_PIN) ? 1 : 0) << 1) |
         (READ_PIN(ENC_BR_PORT, ENC_BRB_PIN) ? 1 : 0);
}

void isrFL(void){ uint8_t ns = rdFL(); encFL += qLUT[(prevFL << 2) | ns]; prevFL = ns; }
void isrFR(void){ uint8_t ns = rdFR(); encFR += qLUT[(prevFR << 2) | ns]; prevFR = ns; }
void isrBL(void){ uint8_t ns = rdBL(); encBL += qLUT[(prevBL << 2) | ns]; prevBL = ns; }
void isrBR(void){ uint8_t ns = rdBR(); encBR += qLUT[(prevBR << 2) | ns]; prevBR = ns; }

/*** MPU9250 I2C ***/
#define MPU_ADDR           (0x68 << 1)  // 7-bit address shifted for I2C
#define REG_PWR_MGMT_1     0x6B
#define REG_SMPLRT_DIV     0x19
#define REG_CONFIG         0x1A
#define REG_GYRO_CONFIG    0x1B
#define REG_ACCEL_CONFIG   0x1C
#define REG_ACCEL_CONFIG2  0x1D
#define REG_ACCEL_XOUT_H   0x3B

#define I2C_TIMEOUT        10000

static void i2c_start(void){
  I2C1->CR1 |= I2C_CR1_START;
  uint32_t timeout = I2C_TIMEOUT;
  while(!(I2C1->SR1 & I2C_SR1_SB) && timeout--);
}

static void i2c_stop(void){
  I2C1->CR1 |= I2C_CR1_STOP;
}

static void i2c_write_addr(uint8_t addr){
  I2C1->DR = addr;
  uint32_t timeout = I2C_TIMEOUT;
  while(!(I2C1->SR1 & I2C_SR1_ADDR) && timeout--);
  (void)I2C1->SR1; (void)I2C1->SR2; // Clear ADDR
}

static void i2c_write_data(uint8_t data){
  I2C1->DR = data;
  uint32_t timeout = I2C_TIMEOUT;
  while(!(I2C1->SR1 & I2C_SR1_TXE) && timeout--);
}

static uint8_t i2c_read_ack(void){
  I2C1->CR1 |= I2C_CR1_ACK;
  uint32_t timeout = I2C_TIMEOUT;
  while(!(I2C1->SR1 & I2C_SR1_RXNE) && timeout--);
  return I2C1->DR;
}

static uint8_t i2c_read_nack(void){
  I2C1->CR1 &= ~I2C_CR1_ACK;
  uint32_t timeout = I2C_TIMEOUT;
  while(!(I2C1->SR1 & I2C_SR1_RXNE) && timeout--);
  return I2C1->DR;
}

static void i2c_write(uint8_t reg, uint8_t val){
  i2c_start();
  i2c_write_addr(MPU_ADDR);
  i2c_write_data(reg);
  i2c_write_data(val);
  i2c_stop();
}

static uint8_t i2c_read_burst(uint8_t reg, uint8_t n, uint8_t *dst){
  i2c_start();
  i2c_write_addr(MPU_ADDR);
  i2c_write_data(reg);

  i2c_start(); // Repeated start
  i2c_write_addr(MPU_ADDR | 1); // Read mode

  for(uint8_t i = 0; i < n; i++){
    if(i == n - 1){
      dst[i] = i2c_read_nack();
    } else {
      dst[i] = i2c_read_ack();
    }
  }

  i2c_stop();
  return n;
}

static void delay_ms(uint32_t ms){
  for(uint32_t i = 0; i < ms; i++){
    for(volatile uint32_t j = 0; j < 8000; j++); // ~1ms @ 72MHz
  }
}

static uint8_t mpu_begin(void){
  i2c_write(REG_PWR_MGMT_1, 0x01);     // Clock PLL, wake
  delay_ms(10);
  i2c_write(REG_SMPLRT_DIV, 9);        // 1kHz/(1+9)=100Hz
  i2c_write(REG_CONFIG, 0x03);         // DLPF ~44Hz
  i2c_write(REG_GYRO_CONFIG, 0x18);    // ±2000 dps
  i2c_write(REG_ACCEL_CONFIG, 0x08);   // ±4g
  i2c_write(REG_ACCEL_CONFIG2, 0x03);  // Accel DLPF
  delay_ms(10);
  return 1;
}

float gyro_bias_z = 0.0f;

static uint8_t mpu_read(float *ax, float *ay, float *az, float *gx, float *gy, float *gz){
  uint8_t buf[14];
  uint8_t ret = i2c_read_burst(REG_ACCEL_XOUT_H, 14, buf);
  if(ret != 14) return 0;

  int16_t ax_raw = (int16_t)((buf[0] << 8) | buf[1]);
  int16_t ay_raw = (int16_t)((buf[2] << 8) | buf[3]);
  int16_t az_raw = (int16_t)((buf[4] << 8) | buf[5]);
  int16_t gx_raw = (int16_t)((buf[8] << 8) | buf[9]);
  int16_t gy_raw = (int16_t)((buf[10] << 8) | buf[11]);
  int16_t gz_raw = (int16_t)((buf[12] << 8) | buf[13]);

  const float ACCEL_S = 9.80665f / 8192.0f;
  *ax = ax_raw * ACCEL_S;
  *ay = ay_raw * ACCEL_S;
  *az = az_raw * ACCEL_S;

  const float GYRO_S = 0.001064225f; // (PI/180.0f) / 16.4f
  *gx = gx_raw * GYRO_S;
  *gy = gy_raw * GYRO_S;
  *gz = gz_raw * GYRO_S - gyro_bias_z;

  return 1;
}

/*** PACKET STRUCTURE ***/
typedef struct __attribute__((packed)) {
  uint32_t t_ms;
  int32_t  ticksFL;
  int32_t  ticksFR;
  int32_t  ticksBL;
  int32_t  ticksBR;
  float    ax, ay, az;
  float    gx, gy, gz;
  uint16_t flags;
} Payload;

static uint8_t checksum_xor(const uint8_t *p, uint16_t n){
  uint8_t c = 0;
  for(uint16_t i = 0; i < n; i++) c ^= p[i];
  return c;
}

static void uart_send_byte(uint8_t b){
  while(!(USART3->SR & USART_SR_TXE));
  USART3->DR = b;
}

static void send_payload(const Payload *p){
  const uint8_t H0 = 0xAA, H1 = 0x55;

  uart_send_byte(H0);
  uart_send_byte(H1);

  const uint8_t *data = (const uint8_t*)p;
  for(uint16_t i = 0; i < sizeof(Payload); i++){
    uart_send_byte(data[i]);
  }

  uint8_t cs = checksum_xor(data, sizeof(Payload));
  uart_send_byte(cs);
}

/*** SYSTICK ***/
volatile uint32_t systick_millis = 0;

void SysTick_Handler(void){
  systick_millis++;
}

static uint32_t millis(void){
  return systick_millis;
}

/*** EXTI HANDLERS ***/
void EXTI9_5_IRQHandler(void){
  if(EXTI->PR & BIT(8)){ EXTI->PR = BIT(8); isrFL(); }  // PA8
  if(EXTI->PR & BIT(9)){ EXTI->PR = BIT(9); isrFL(); }  // PA9
}

void EXTI15_10_IRQHandler(void){
  if(EXTI->PR & BIT(10)){ EXTI->PR = BIT(10); isrBL(); } // PA10
  if(EXTI->PR & BIT(11)){ EXTI->PR = BIT(11); isrBL(); } // PA11
  if(EXTI->PR & BIT(15)){ EXTI->PR = BIT(15); isrFR(); } // PA15
}

void EXTI3_IRQHandler(void){
  if(EXTI->PR & BIT(3)){ EXTI->PR = BIT(3); isrFR(); }   // PB3
}

void EXTI4_IRQHandler(void){
  if(EXTI->PR & BIT(4)){ EXTI->PR = BIT(4); isrBR(); }   // PB4
}

void EXTI9_5_IRQHandler_PB5(void){ // Actually shares EXTI9_5
  if(EXTI->PR & BIT(5)){ EXTI->PR = BIT(5); isrBR(); }   // PB5
}

/*** INITIALIZATION ***/
static void clock_init(void){
  // Enable HSE
  RCC->CR |= RCC_CR_HSEON;
  while(!(RCC->CR & RCC_CR_HSERDY));

  // Configure PLL: HSE * 9 = 72MHz
  RCC->CFGR |= RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9;
  RCC->CR |= RCC_CR_PLLON;
  while(!(RCC->CR & RCC_CR_PLLRDY));

  // Flash latency for 72MHz
  FLASH->ACR |= FLASH_ACR_LATENCY_2;

  // Select PLL as system clock
  RCC->CFGR |= RCC_CFGR_SW_PLL;
  while((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);

  // Enable peripheral clocks
  RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_AFIOEN;
  RCC->APB1ENR |= RCC_APB1ENR_USART3EN | RCC_APB1ENR_I2C1EN;
}

static void gpio_init(void){
  // Free JTAG pins (keep SWD)
  AFIO->MAPR |= AFIO_MAPR_SWJ_CFG_JTAGDISABLE;

  // Configure encoder pins as input with pull-up
  // PA8-11: CNF=10 (input pull-up/down), MODE=00 (input), then set ODR for pull-up
  GPIOA->CRH &= ~(0xFFFF << 0);  // Clear PA8-11
  GPIOA->CRH |= (0x8888 << 0);   // Input pull-up/down
  GPIOA->ODR |= BIT(8) | BIT(9) | BIT(10) | BIT(11); // Pull-up

  // PA15: Input pull-up
  GPIOA->CRH &= ~(0xF << 28);
  GPIOA->CRH |= (0x8 << 28);
  GPIOA->ODR |= BIT(15);

  // PB3-5: Input pull-up
  GPIOB->CRL &= ~(0xFFF << 12);
  GPIOB->CRL |= (0x888 << 12);
  GPIOB->ODR |= BIT(3) | BIT(4) | BIT(5);

  // I2C pins: Alternate function open-drain
  GPIOB->CRL &= ~(0xFF << 24);
  GPIOB->CRL |= (0xFF << 24); // AF open-drain, 50MHz

  // UART pins: TX=AF push-pull, RX=input floating
  GPIOB->CRH &= ~(0xFF << 8);
  GPIOB->CRH |= (0xB << 8) | (0x4 << 12); // PB10=AF PP, PB11=input
}

static void exti_init(void){
  // Map EXTI lines to pins
  AFIO->EXTICR[2] |= (0 << 0) | (0 << 4);     // EXTI8-9 = PA
  AFIO->EXTICR[2] |= (0 << 8) | (0 << 12);    // EXTI10-11 = PA
  AFIO->EXTICR[3] |= (0 << 12);               // EXTI15 = PA
  AFIO->EXTICR[0] |= (1 << 12);               // EXTI3 = PB
  AFIO->EXTICR[1] |= (1 << 0) | (1 << 4);     // EXTI4-5 = PB

  // Enable rising and falling edge triggers
  EXTI->RTSR |= BIT(8) | BIT(9) | BIT(10) | BIT(11) | BIT(15) | BIT(3) | BIT(4) | BIT(5);
  EXTI->FTSR |= BIT(8) | BIT(9) | BIT(10) | BIT(11) | BIT(15) | BIT(3) | BIT(4) | BIT(5);

  // Unmask interrupts
  EXTI->IMR |= BIT(8) | BIT(9) | BIT(10) | BIT(11) | BIT(15) | BIT(3) | BIT(4) | BIT(5);

  // Enable NVIC
  NVIC_EnableIRQ(EXTI3_IRQn);
  NVIC_EnableIRQ(EXTI4_IRQn);
  NVIC_EnableIRQ(EXTI9_5_IRQn);
  NVIC_EnableIRQ(EXTI15_10_IRQn);
}

static void uart_init(void){
  // Baud rate: 72MHz / (16 * 921600) = 4.88 ≈ 5
  USART3->BRR = 78; // More accurate for 921600
  USART3->CR1 = USART_CR1_TE | USART_CR1_UE; // Enable TX and USART
}

static void i2c_init(void){
  I2C1->CR1 = I2C_CR1_SWRST;
  I2C1->CR1 = 0;

  // Configure for 100kHz: PCLK1=36MHz, FREQ=36
  I2C1->CR2 = 36;
  I2C1->CCR = 180; // 36MHz / (2 * 100kHz) = 180
  I2C1->TRISE = 37; // Max rise time

  I2C1->CR1 = I2C_CR1_PE; // Enable I2C
}

static void systick_init(void){
  SysTick->LOAD = 72000 - 1; // 1ms @ 72MHz
  SysTick->VAL = 0;
  SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
}

/*** MAIN ***/
int main(void){
  clock_init();
  gpio_init();
  systick_init();
  uart_init();
  i2c_init();

  delay_ms(100);

  if(!mpu_begin()){
    while(1); // Hang on MPU init failure
  }

  // Calibrate gyro Z bias
  float sum = 0;
  const uint16_t N = 400;
  for(uint16_t i = 0; i < N; i++){
    float ax, ay, az, gx, gy, gz;
    if(mpu_read(&ax, &ay, &az, &gx, &gy, &gz)){
      sum += (gz + gyro_bias_z);
    }
    delay_ms(2);
  }
  gyro_bias_z = sum / N;

  // Capture initial encoder states
  prevFL = rdFL(); prevFR = rdFR(); prevBL = rdBL(); prevBR = rdBR();

  // Enable EXTI
  exti_init();

  uint32_t last = 0;

  while(1){
    uint32_t now = millis();
    if(now - last < 10) continue; // 100 Hz
    last = now;

    // Snapshot encoder counts
    int32_t tFL, tFR, tBL, tBR;
    __disable_irq();
    tFL = encFL; tFR = encFR; tBL = encBL; tBR = encBR;
    __enable_irq();

    // Read IMU
    float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
    uint8_t ok = mpu_read(&ax, &ay, &az, &gx, &gy, &gz);

    // Build payload
    Payload p;
    p.t_ms = now;
    p.ticksFL = tFL; p.ticksFR = tFR; p.ticksBL = tBL; p.ticksBR = tBR;
    p.ax = ax; p.ay = ay; p.az = az;
    p.gx = gx; p.gy = gy; p.gz = gz;
    p.flags = ok ? 0 : 0x0001;

    send_payload(&p);
  }

  return 0;
}
