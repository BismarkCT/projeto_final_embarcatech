#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "pico_lfs.h" // Biblioteca para LittleFS

// Configurações de áudio
#define AUDIO_PIN 26
#define SAMPLE_RATE 22050
#define DURATION_SECONDS 5
#define NUM_SAMPLES (SAMPLE_RATE * DURATION_SECONDS)

// Configurações do LittleFS: usaremos os últimos 256KB da flash
#define FS_OFFSET (PICO_FLASH_SIZE_BYTES - (256 * 1024))
#define FS_SIZE (256 * 1024)

// Buffer para armazenar as amostras de áudio capturadas (valor bruto de 12 bits)
uint16_t audio_buffer[NUM_SAMPLES];

// Estrutura do cabeçalho WAV
typedef struct
{
  uint8_t RIFF[4];        // "RIFF"
  uint32_t ChunkSize;     // 36 + SubChunk2Size
  uint8_t WAVE[4];        // "WAVE"
  uint8_t fmt[4];         // "fmt "
  uint32_t SubChunk1Size; // 16 para PCM
  uint16_t AudioFormat;   // PCM = 1
  uint16_t NumChannels;   // Mono = 1
  uint32_t SampleRate;    // e.g., 22050
  uint32_t ByteRate;      // SampleRate * NumChannels * BitsPerSample/8
  uint16_t BlockAlign;    // NumChannels * BitsPerSample/8
  uint16_t BitsPerSample; // 16 bits
  uint8_t SubChunk2ID[4]; // "data"
  uint32_t SubChunk2Size; // NUM_SAMPLES * NumChannels * BitsPerSample/8
} WAVHeader;

// Variáveis globais do LittleFS
lfs_t lfs;
struct lfs_config *lfs_cfg;

// Função para escrever o cabeçalho WAV usando a API do LittleFS
void write_wav_header_lfs(lfs_file_t *file, uint32_t sample_rate, uint16_t bits_per_sample, uint16_t num_channels, uint32_t num_samples)
{
  WAVHeader header;
  memcpy(header.RIFF, "RIFF", 4);
  header.ChunkSize = 36 + num_samples * num_channels * (bits_per_sample / 8);
  memcpy(header.WAVE, "WAVE", 4);
  memcpy(header.fmt, "fmt ", 4);
  header.SubChunk1Size = 16;
  header.AudioFormat = 1; // PCM
  header.NumChannels = num_channels;
  header.SampleRate = sample_rate;
  header.ByteRate = sample_rate * num_channels * (bits_per_sample / 8);
  header.BlockAlign = num_channels * (bits_per_sample / 8);
  header.BitsPerSample = bits_per_sample;
  memcpy(header.SubChunk2ID, "data", 4);
  header.SubChunk2Size = num_samples * num_channels * (bits_per_sample / 8);

  lfs_file_write(&lfs, file, &header, sizeof(WAVHeader));
}

// Inicializa o sistema de arquivos LittleFS
void init_filesystem()
{
  lfs_cfg = pico_lfs_init(FS_OFFSET, FS_SIZE);
  if (!lfs_cfg)
  {
    panic("Erro ao inicializar LittleFS");
  }

  int err = lfs_mount(&lfs, lfs_cfg);
  if (err < 0)
  {
    // Se não conseguir montar, formata e tenta novamente
    err = lfs_format(&lfs, lfs_cfg);
    if (err < 0)
    {
      panic("Falha ao formatar o sistema de arquivos");
    }
    err = lfs_mount(&lfs, lfs_cfg);
    if (err < 0)
    {
      panic("Falha ao montar o sistema de arquivos");
    }
  }
}

// Captura áudio usando ADC e DMA para preencher o buffer
void capture_audio_dma(uint16_t *buffer, uint32_t num_samples)
{
  // Inicializa o ADC e o pino correspondente
  adc_init();
  adc_gpio_init(AUDIO_PIN);
  adc_select_input(0);

  // Configura o FIFO do ADC:
  // - O FIFO receberá cada conversão completa
  // - A flag DMA (DREQ) será acionada com 1 amostra disponível
  adc_fifo_setup(true, true, 1, false, false);

  // Configura o divisor de clock para atingir a taxa de amostragem desejada
  adc_set_clkdiv((float)clock_get_hz(clk_adc) / (float)SAMPLE_RATE);

  // Configura o canal DMA
  int dma_chan = dma_claim_unused_channel(true);
  dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
  channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
  channel_config_set_read_increment(&cfg, false); // Fonte fixa: FIFO do ADC
  channel_config_set_write_increment(&cfg, true); // Buffer de destino é incremental
  channel_config_set_dreq(&cfg, DREQ_ADC);        // Solicitação proveniente do ADC

  dma_channel_configure(
      dma_chan,
      &cfg,
      buffer,        // Destino: buffer na RAM
      &adc_hw->fifo, // Fonte: FIFO do ADC
      num_samples,   // Número de transferências
      true           // Inicia imediatamente
  );

  adc_run(true);
  // Aguarda a finalização da transferência DMA
  dma_channel_wait_for_finish_blocking(dma_chan);
  adc_run(false);
}

int main()
{
  stdio_init_all();
  // Aguarda um pouco para que a saída padrão seja inicializada (para debug via USB/serial)
  sleep_ms(2000);

  // Inicializa o sistema de arquivos LittleFS
  init_filesystem();

  printf("Capturando áudio...\n");
  capture_audio_dma(audio_buffer, NUM_SAMPLES);
  printf("Captura concluída.\n");

  // Abre o arquivo "audio.wav" para escrita (modo binário) usando a API do LittleFS
  lfs_file_t file;
  int err = lfs_file_open(&lfs, &file, "audio.wav", LFS_O_WRONLY | LFS_O_CREAT);
  if (err < 0)
  {
    printf("Erro ao abrir o arquivo para escrita.\n");
    return 1;
  }

  // Escreve o cabeçalho WAV no arquivo
  write_wav_header_lfs(&file, SAMPLE_RATE, 16, 1, NUM_SAMPLES);

  // Como o ADC gera valores de 12 bits (0–4095) e queremos 16 bits, fazemos o escalonamento
  uint16_t scaled_buffer[NUM_SAMPLES];
  for (uint32_t i = 0; i < NUM_SAMPLES; i++)
  {
    // Multiplica por 16 para converter de 12 bits para 16 bits
    scaled_buffer[i] = audio_buffer[i] << 4;
  }

  // Escreve os dados de áudio (convertidos) no arquivo
  lfs_file_write(&lfs, &file, scaled_buffer, sizeof(scaled_buffer));
  lfs_file_close(&lfs, &file);

  // Desmonta o sistema de arquivos (opcional)
  lfs_unmount(&lfs);

  printf("Gravação concluída com sucesso.\n");

  while (true)
  {
    tight_loop_contents();
  }

  return 0;
}
