#include "stm32f4xx.h"
#include "w25q128.h"

// ���ļ�ΪW25Q128������ʵ�֣��ṩ�����Ķ�д�Ͳ�������
// W25Q128_1
// SCK��PA5
// MISO��PA6
// MOSI��PA7
// CS��PE13
// SPI1
// TX��DMA2-STREAM3-CH3
// RX��DMA2-STREAM0-CH3

// SPI���ͨ��Ƶ��104M
// JEDEC-ID��EF 40 18

#define W25Q_CS_LOW()   GPIO_ResetBits(GPIOE, GPIO_Pin_13)
#define W25Q_CS_HIGH()  GPIO_SetBits(GPIOE, GPIO_Pin_13)

// W25Qxx ָ���
#define W25Q_WriteEnable        0x06
#define W25Q_ReadStatusReg1     0x05
#define W25Q_ReadData           0x03
#define W25Q_PageProgram        0x02
#define W25Q_SectorErase        0x20

void w25qxx_io_init(void)
{
    // ����CS����Ϊ���������
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);
    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_13;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_100MHz; // W25Q128���֧��104MHz����������Ϊ100MHz��ȷ��SPIʱ���ȶ�
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOE, &GPIO_InitStruct);
    W25Q_CS_HIGH(); // Ĭ��CS���ߣ�W25Q128��ѡ��

    // ����SPI����Ϊ���ù���
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource6, GPIO_AF_SPI1); // MISO
	GPIO_PinAFConfig(GPIOA, GPIO_PinSource7, GPIO_AF_SPI1); // MOSI
	GPIO_PinAFConfig(GPIOA, GPIO_PinSource5, GPIO_AF_SPI1); // SCK

    // ����SPI����
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
	SPI_InitStruct.SPI_CPOL = SPI_CPOL_Low;// ʱ�ӿ��е͵�ƽ
	SPI_InitStruct.SPI_CPHA = SPI_CPHA_1Edge;// �����ڵ�һ��ʱ���ز���
	SPI_InitStruct.SPI_NSS = SPI_NSS_Soft;
	SPI_InitStruct.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8; // 84MHz/8=10.5MHz
	SPI_InitStruct.SPI_FirstBit = SPI_FirstBit_MSB;
	SPI_Init(SPI1, &SPI_InitStruct);
	//SPI_DMACmd(SPI1, SPI_DMAReq_Tx, ENABLE);
	SPI_Cmd(SPI1, ENABLE);
}

/**
 * @brief SPI�ײ��շ�һ���ֽ�
 * @param tx_data Ҫ���͵�����
 * @return ���յ�������
 * @note �ú����ڷ�������ʱ��ȴ����ͻ������գ����ڽ�������ʱ�ȴ����ջ������ǿգ���ȷ�����ݵ���ȷ���䡣
 */
static uint8_t w25qxx_spi_read_write_byte(uint8_t tx_data)
{
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET); // �ȴ����ͻ�������
    SPI_I2S_SendData(SPI1, tx_data);

    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET); // �ȴ����ջ������ǿ�
    return SPI_I2S_ReceiveData(SPI1);
}

/**
 * @brief �ȴ�W25Q128�ڲ�������ɣ�æ�ȴ���
 */
static void w25qxx_wait_busy(void)
{
    uint8_t status;
    do
    {
        W25Q_CS_LOW();
        w25qxx_spi_read_write_byte(W25Q_ReadStatusReg1); // ���Ͷ�ȡ״̬�Ĵ�����ָ��
        status = w25qxx_spi_read_write_byte(0xFF); // ����һ����������ֽ��Զ�ȡ״̬�Ĵ�����ֵ
        W25Q_CS_HIGH();
    } while (status & 0x01); // ���æ��־λ��Bit0�������Ϊ1��ʾW25Q128����æµ�������ȴ�
}

void w25qxx_init(void)
{
    w25qxx_io_init();
    w25qxx_spi_init();
}

/**
 * @brief  дʹ�ܣ���ִ��д��������ҳ��̻�����������֮ǰ�������ȷ���дʹ��ָ����ʹW25Q128�����д״̬��
 */
static void w25qxx_write_enable(void)
{
    W25Q_CS_LOW();
    w25qxx_spi_read_write_byte(W25Q_WriteEnable); // ����дʹ��ָ��
    W25Q_CS_HIGH();
}

/**
 * @brief ��ȡ���ݣ�֧��������ҳ��ȡ��
 *
 * @param addr ��ʼ��ַ��24λ��
 * @param buf ���ݻ�����
 * @param len Ҫ��ȡ���ֽ���
 */
void w25qxx_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    w25qxx_wait_busy(); // ȷ��W25Q128��æ
    W25Q_CS_LOW();
    w25qxx_spi_read_write_byte(W25Q_ReadData); // ���Ͷ�ȡ���ݵ�ָ��
    w25qxx_spi_read_write_byte((uint8_t)(addr >> 16)); // ���͵�ַ�ĸ�8λ
    w25qxx_spi_read_write_byte((uint8_t)(addr >> 8));  // ���͵�ַ����8λ
    w25qxx_spi_read_write_byte((uint8_t)(addr));       // ���͵�ַ�ĵ�8λ

    for (uint32_t i = 0; i < len; i++)
    {
        buf[i] = w25qxx_spi_read_write_byte(0xFF); // ����һ����������ֽ��Զ�ȡ����
    }
    W25Q_CS_HIGH();
}

/**
 * @brief д��һҳ���ݣ��������д256�ֽڣ��ұ�����ͬһҳ�ڣ�
 * @param addr ��ʼ��ַ��24λ��
 * @param buf ���ݻ�����
 * @param len Ҫд����ֽ��������256�ֽڣ�
 */
static void w25qxx_page_program(uint32_t addr, uint8_t *buf, uint32_t len)
{
    w25qxx_wait_busy(); // ȷ��W25Q128��æ
    w25qxx_write_enable(); // ����дʹ��ָ��

    W25Q_CS_LOW();
    w25qxx_spi_read_write_byte(W25Q_PageProgram); // ����ҳ���ָ��
    w25qxx_spi_read_write_byte((uint8_t)(addr >> 16)); // ���͵�ַ�ĸ�8λ
    w25qxx_spi_read_write_byte((uint8_t)(addr >> 8));  // ���͵�ַ����8λ
    w25qxx_spi_read_write_byte((uint8_t)(addr));       // ���͵�ַ�ĵ�8λ

    for (uint32_t i = 0; i < len; i++)
    {
        w25qxx_spi_read_write_byte(buf[i]); // ��������
    }
    W25Q_CS_HIGH();
    w25qxx_wait_busy(); // �ȴ�д�����
}

/**
 * @brief д�����ⳤ�ȵ����ݣ��Զ�������ҳд��
 * @param addr ��ʼ��ַ��24λ��
 * @param buf ���ݻ�����
 * @param len Ҫд����ֽ���
 */
void w25qxx_write(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint32_t page_offset = addr % 256; // ���㵱ǰ��ַ��ҳ�ڵ�ƫ��
    uint32_t bytes_to_write;

    while (len > 0)
    {
        bytes_to_write = (page_offset + len > 256) ? (256 - page_offset) : len; // ���㱾��д����ֽ�����ȷ������ҳ

        w25qxx_page_program(addr, buf, bytes_to_write); // д��һҳ����

        addr += bytes_to_write; // ���µ�ַ
        buf += bytes_to_write;  // ��������ָ��
        len -= bytes_to_write;  // ����ʣ�೤��
        page_offset = 0;        // ����д���ҳ����ʼλ�ÿ�ʼ
    }
}
/**
 * @brief ����һ��������ͨ��Ϊ4KB��
 * @param addr �����ڵ������ַ��ͨ������24λ�����׵�ַ��
 */
void w25qxx_erase_sector(uint32_t addr)
{
    w25qxx_wait_busy(); // ȷ��W25Q128��æ
    w25qxx_write_enable(); // ����дʹ��ָ��

    W25Q_CS_LOW();
    w25qxx_spi_read_write_byte(W25Q_SectorErase); // ������������ָ��
    w25qxx_spi_read_write_byte((uint8_t)(addr >> 16)); // ���͵�ַ�ĸ�8λ
    w25qxx_spi_read_write_byte((uint8_t)(addr >> 8));  // ���͵�ַ����8λ
    w25qxx_spi_read_write_byte((uint8_t)(addr));       // ���͵�ַ�ĵ�8λ
    W25Q_CS_HIGH();

    w25qxx_wait_busy(); // �����ǳ���ʱ��Լ100~400ms��������ȴ������������
}
