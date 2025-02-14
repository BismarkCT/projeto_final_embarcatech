
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "neopixel.c"

typedef struct
{
    // "RIFF" Chunk Descriptor
    uint8_t RIFF[4];    // "RIFF"
    uint32_t ChunkSize; // 4 + (8 + SubChunk1Size) + (8 + SubChunk2Size)
    uint8_t WAVE[4];    // "WAVE"
    // "fmt " Sub-chunk
    uint8_t fmt[4];         // "fmt "
    uint32_t SubChunk1Size; // 16 for PCM
    uint16_t AudioFormat;   // PCM = 1
    uint16_t NumChannels;   // Mono = 1, Stereo = 2
    uint32_t SampleRate;    // e.g., 22050, 44100, etc.
    uint32_t ByteRate;      // SampleRate * NumChannels * BitsPerSample/8
    uint16_t BlockAlign;    // NumChannels * BitsPerSample/8
    uint16_t BitsPerSample; // e.g., 8 bits = 8, 16 bits = 16
    // "data" Sub-chunk
    uint8_t SubChunk2ID[4]; // "data"
    uint32_t SubChunk2Size; // NumSamples * NumChannels * BitsPerSample/8
} WAVHeader;

void write_wav_header(FILE *file, uint32_t sample_rate, uint16_t bits_per_sample, uint16_t num_channels, uint32_t num_samples)
{
    WAVHeader header;
    // Preenchendo o cabeçalho com os valores apropriados
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

    // Escrevendo o cabeçalho no arquivo
    fwrite(&header, sizeof(WAVHeader), 1, file);
}

// Pino e canal do microfone no ADC.
#define MIC_CHANNEL 2
#define MIC_PIN (26 + MIC_CHANNEL)
#define SAMPLE_RATE 22050
#define BITS_PER_SAMPLE 16
#define NUM_CHANNELS 1
#define DURATION_SECONDS 5
#define NUM_SAMPLES (SAMPLE_RATE * DURATION_SECONDS)

int main()
{
    stdio_init_all();

    sleep_ms(10000);

    printf("Vai começar\n");

    // Inicialização do ADC
    adc_init();
    adc_gpio_init(MIC_PIN);
    adc_select_input(MIC_CHANNEL);

    // Configuração do FIFO do ADC
    adc_fifo_setup(
        true,  // Escrever cada conversão completa no FIFO
        true,  // Habilitar solicitação de DMA (DREQ)
        1,     // DREQ acionado quando pelo menos 1 amostra presente
        false, // Desabilitar o bit de erro
        true   // Shiftar cada amostra para 8 bits ao empurrar para o FIFO
    );

    // Configuração do divisor de clock do ADC para obter a taxa de amostragem desejada
    adc_set_clkdiv((float)clock_get_hz(clk_adc) / (float)SAMPLE_RATE);

    // Buffer para armazenar as amostras de áudio
    uint16_t audio_buffer[NUM_SAMPLES];

    // Configuração do DMA
    int dma_chan = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, DREQ_ADC);

    dma_channel_configure(
        dma_chan,
        &cfg,
        audio_buffer,  // Destino
        &adc_hw->fifo, // Fonte
        NUM_SAMPLES,   // Número de transferências
        true           // Iniciar imediatamente
    );

    // Iniciar o ADC
    adc_run(true);

    // Esperar até que o DMA complete a transferência
    dma_channel_wait_for_finish_blocking(dma_chan);

    // Parar o ADC
    adc_run(false);

    // Abertura do arquivo para escrita
    FILE *file = fopen("audio.wav", "wb");
    if (!file)
    {
        printf("Erro ao abrir o arquivo para escrita.\n");
        return 1;
    }

    // Escrita do cabeçalho WAV
    write_wav_header(file, SAMPLE_RATE, BITS_PER_SAMPLE, NUM_CHANNELS, NUM_SAMPLES);

    // Escrita das amostras de áudio no arquivo
    fwrite(audio_buffer, sizeof(uint16_t), NUM_SAMPLES, file);

    // Fechamento do arquivo
    fclose(file);

    printf("Gravação concluída com sucesso.\n");
    return 0;
}
