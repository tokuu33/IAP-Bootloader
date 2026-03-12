#include "stm32f4xx.h"
#include "w25q128.h"

// 该文件为W25Q128的驱动实现，提供基本的读写和擦除功能
// W25Q128_1
// SCK：PA5
// MISO：PA6
// MOSI：PA7
// CS：PE13
// SPI1
// TX：DMA2-STREAM3-CH3
// RX：DMA2-STREAM0-CH3

// SPI最大通信频率104M
// JEDEC-ID：EF 40 18

#define W25Q_CS_LOW()   GPIO_ResetBits(GPIOE, GPIO_Pin_13)
#define W25Q_CS_HIGH()  GPIO_SetBits(GPIOE, GPIO_Pin_13)

// W25Qxx 指令表
#define W25Q_WriteEnable        0x06
#define W25Q_ReadStatusReg1     0x05
#define W25Q_ReadData           0x03
#define W25Q_PageProgram        0x02
#define W25Q_SectorErase        0x20

void w25qxx_io_init(void)
{
    // 配置CS引脚为输出并拉高
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);
    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_13;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_100MHz; // W25Q128最高支持104MHz，这里设置为100MHz以确保SPI时钟稳定
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOE, &GPIO_InitStruct);
    W25Q_CS_HIGH(); // 默认CS拉高，W25Q128不选中

    // 配置SPI引脚为复用功能
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource6, GPIO_AF_SPI1); // MISO
	GPIO_PinAFConfig(GPIOA, GPIO_PinSource7, GPIO_AF_SPI1); // MOSI
	GPIO_PinAFConfig(GPIOA, GPIO_PinSource5, GPIO_AF_SPI1); // SCK

    // 配置SPI引脚
    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(GPIOA, &GPIO_InitStruct);
}
void w25qxx_spi_init(void)
{
	SPI_InitTypeDef SPI_InitStruct;
	SPI_StructInit(&SPI_InitStruct);
	SPI_InitStruct.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
	SPI_InitStruct.SPI_Mode = SPI_Mode_Master;
	SPI_InitStruct.SPI_DataSize = SPI_DataSize_8b;
	SPI_InitStruct.SPI_CPOL = SPI_CPOL_Low;// 时钟空闲低电平
	SPI_InitStruct.SPI_CPHA = SPI_CPHA_1Edge;// 数据在第一个时钟沿采样
	SPI_InitStruct.SPI_NSS = SPI_NSS_Soft;
	SPI_InitStruct.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8; // 84MHz/8=10.5MHz
	SPI_InitStruct.SPI_FirstBit = SPI_FirstBit_MSB;
	SPI_Init(SPI1, &SPI_InitStruct);
	//SPI_DMACmd(SPI1, SPI_DMAReq_Tx, ENABLE);
	SPI_Cmd(SPI1, ENABLE);
}

/**
 * @brief SPI底层收发一个字节
 * @param tx_data 要发送的数据
 * @return 接收到的数据
 * @note 该函数在发送数据时会等待发送缓冲区空，并在接收数据时等待接收缓冲区非空，以确保数据的正确传输。
 */
static uint8_t w25qxx_spi_read_write_byte(uint8_t tx_data)
{
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET); // 等待发送缓冲区空
    SPI_I2S_SendData(SPI1, tx_data);

    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET); // 等待接收缓冲区非空
    return SPI_I2S_ReceiveData(SPI1);
}

/**
 * @brief 等待W25Q128内部操作完成（忙等待）
 */
static void w25qxx_wait_busy(void)
{
    uint8_t status;
    do
    {
        W25Q_CS_LOW();
        w25qxx_spi_read_write_byte(W25Q_ReadStatusReg1); // 发送读取状态寄存器的指令
        status = w25qxx_spi_read_write_byte(0xFF); // 发送一个无意义的字节以读取状态寄存器的值
        W25Q_CS_HIGH();
    } while (status & 0x01); // 检查忙标志位（Bit0），如果为1表示W25Q128正在忙碌，继续等待
}

void w25qxx_init(void)
{
    w25qxx_io_init();
    w25qxx_spi_init();
}

/**
 * @brief  写使能：在执行写操作（如页编程或扇区擦除）之前，必须先发送写使能指令以使W25Q128进入可写状态。
 */
static void w25qxx_write_enable(void)
{
    W25Q_CS_LOW();
    w25qxx_spi_read_write_byte(W25Q_WriteEnable); // 发送写使能指令
    W25Q_CS_HIGH();
}

/**
 * @brief 读取数据（支持连续跨页读取）
 *
 * @param addr 起始地址（24位）
 * @param buf 数据缓冲区
 * @param len 要读取的字节数
 */
void w25qxx_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    w25qxx_wait_busy(); // 确保W25Q128不忙
    W25Q_CS_LOW();
    w25qxx_spi_read_write_byte(W25Q_ReadData); // 发送读取数据的指令
    w25qxx_spi_read_write_byte((uint8_t)(addr >> 16)); // 发送地址的高8位
    w25qxx_spi_read_write_byte((uint8_t)(addr >> 8));  // 发送地址的中8位
    w25qxx_spi_read_write_byte((uint8_t)(addr));       // 发送地址的低8位

    for (uint32_t i = 0; i < len; i++)
    {
        buf[i] = w25qxx_spi_read_write_byte(0xFF); // 发送一个无意义的字节以读取数据
    }
    W25Q_CS_HIGH();
}

/**
 * @brief 写入一页数据（单次最多写256字节，且必须在同一页内）
 * @param addr 起始地址（24位）
 * @param buf 数据缓冲区
 * @param len 要写入的字节数（最多256字节）
 */
static void w25qxx_page_program(uint32_t addr, uint8_t *buf, uint32_t len)
{
    w25qxx_wait_busy(); // 确保W25Q128不忙
    w25qxx_write_enable(); // 发送写使能指令

    W25Q_CS_LOW();
    w25qxx_spi_read_write_byte(W25Q_PageProgram); // 发送页编程指令
    w25qxx_spi_read_write_byte((uint8_t)(addr >> 16)); // 发送地址的高8位
    w25qxx_spi_read_write_byte((uint8_t)(addr >> 8));  // 发送地址的中8位
    w25qxx_spi_read_write_byte((uint8_t)(addr));       // 发送地址的低8位

    for (uint32_t i = 0; i < len; i++)
    {
        w25qxx_spi_read_write_byte(buf[i]); // 发送数据
    }
    W25Q_CS_HIGH();
    w25qxx_wait_busy(); // 等待写入完成
}

/**
 * @brief 写入任意长度的数据，自动处理跨页写入
 * @param addr 起始地址（24位）
 * @param buf 数据缓冲区
 * @param len 要写入的字节数
 */
void w25qxx_write(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint32_t page_offset = addr % 256; // 计算当前地址在页内的偏移
    uint32_t bytes_to_write;

    while (len > 0)
    {
        bytes_to_write = (page_offset + len > 256) ? (256 - page_offset) : len; // 计算本次写入的字节数，确保不跨页

        w25qxx_page_program(addr, buf, bytes_to_write); // 写入一页数据

        addr += bytes_to_write; // 更新地址
        buf += bytes_to_write;  // 更新数据指针
        len -= bytes_to_write;  // 更新剩余长度
        page_offset = 0;        // 后续写入从页的起始位置开始
    }
}
/**
 * @brief 擦除一个扇区（通常为4KB）
 * @param addr 扇区内的任意地址（通常传入24位扇区首地址）
 */
void w25qxx_erase_sector(uint32_t addr)
{
    w25qxx_wait_busy(); // 确保W25Q128不忙
    w25qxx_write_enable(); // 发送写使能指令

    W25Q_CS_LOW();
    w25qxx_spi_read_write_byte(W25Q_SectorErase); // 发送扇区擦除指令
    w25qxx_spi_read_write_byte((uint8_t)(addr >> 16)); // 发送地址的高8位
    w25qxx_spi_read_write_byte((uint8_t)(addr >> 8));  // 发送地址的中8位
    w25qxx_spi_read_write_byte((uint8_t)(addr));       // 发送地址的低8位
    W25Q_CS_HIGH();

    w25qxx_wait_busy(); // 擦除非常耗时（约100~400ms），必须等待擦除操作完成
}
