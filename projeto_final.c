#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/i2c.h"
#include "hardware/clocks.h"
#include "inc/ssd1306.h"

// Definições de pinos e configurações
#define BUTTON_A 5                       // GPIO5 corresponde ao Botão A da BitDogLab
#define BUTTON_B 6                       // GPIO6 corresponde ao Botão B da BitDogLab
#define BUZZER_PIN_A 21                  // GPIO21 corresponde ao Buzzer A da BitDogLab
#define BUZZER_PIN_B 10                  // GPIO10 corresponde ao Buzzer B da BitDogLab
#define MIC_PIN 28                       // GPIO28 corresponde ao Microfone da BitDogLab
#define MIC_CHANNEL 2                    // Corresponde ao canal do ADC do GPIO28
#define I2C_SDA 14                       // GPIO14 corresponde ao SDA do Display OLED da BitDogLab
#define I2C_SCL 15                       // GPIO15 corresponde ao SCL do Display OLED da BitDogLab
#define I2C_PORT i2c1                    // Corresponde ao I2C dos GPIO14 e GPIO15
#define SAMPLE_RATE 12000                // Taxa de amostragem de 12 kHz
#define BUFFER_SIZE (SAMPLE_RATE * 5)    // Buffer para 5 segundos de áudio
#define DELAY_SAMPLE (1e6 / SAMPLE_RATE) // Delay de cada amostra
#define DEBOUNCE_DELAY_MS 200            // Definição de debounce (em milissegundos) dos Botões
#define JOYSTICK_Y 26                    // GPIO26 corresponde ao Joystick no Eixo Y da BitDogLab
#define JOYSTICK_X 27                    // GPIO27 corresponde ao Joystick no Eixo X da BitDogLab
#define JOYSTICK_Y_CHANNEL 0             // Corresponde ao canal do ADC do GPIO26 da BitDogLab
#define JOYSTICK_X_CHANNEL 1             // Corresponde ao canal do ADC do GPIO27 da BitDogLab
#define JOYSTICK_BUTTON 22               // GPIO22 corresponde ao Botão do Joystick da BitDogLab

// Variáveis para debounce dos Botões
volatile absolute_time_t last_button_A_press = {0};
volatile absolute_time_t last_button_B_press = {0};
volatile absolute_time_t last_button_JOYSTICK_press = {0};

// Variáveis para usar nos offsets de mudança de voz
uint frequency_offset = 2400;
uint volume_offset = 0;
int delay_offset = 0;

// Variavel utilizada para fazer a configuração das variaveis de offset
bool config_menu = false;

// Definição dos estados do sistema
typedef enum
{
    STATE_IDLE,
    STATE_INIT,
    STATE_RECORDING,
    STATE_PLAYING,
    STATE_MENU
} system_state_t;
volatile system_state_t system_state = STATE_INIT;

// Variavel utilizada como o tamanho do buffer para a gravação do audio
uint16_t audio_buffer[BUFFER_SIZE];

// Função para configurar ADC com DMA
void config_dma_mic(int dma_chan)
{
    // Configuração do canal DMA
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16); // Transferência de 16 bits
    channel_config_set_read_increment(&cfg, false);           // Leitura fixa (FIFO do ADC)
    channel_config_set_write_increment(&cfg, true);           // Escrita incremental (buffer de áudio)
    channel_config_set_dreq(&cfg, DREQ_ADC);                  // Sincronização com ADC

    dma_channel_configure(
        dma_chan,
        &cfg,
        audio_buffer,  // Destino: buffer de áudio
        &adc_hw->fifo, // Origem: FIFO do ADC
        BUFFER_SIZE,   // Número de transferências
        false          // Não inicia imediatamente
    );
}

// Função de gravação de áudio utilizando DMA
void record_audio()
{
    adc_select_input(MIC_CHANNEL); // Selecionar o canal do ADC que vai pegar os dados

    int dma_chan = dma_claim_unused_channel(true); // Pega o DMA que não esta sendo usado pelo canal
    if (dma_chan < 0)
    {
        printf("Erro: Not found DMA available.\n");
        return;
    }
    config_dma_mic(dma_chan); // Chama a função para configurar o Microfone

    // Inicia a transferência DMA e o ADC
    dma_channel_start(dma_chan);
    adc_run(true);

    // Aguarda a conclusão da transferência DMA (bloqueante)
    dma_channel_wait_for_finish_blocking(dma_chan);

    // Para o ADC e libera o canal DMA
    adc_run(false);
    dma_channel_unclaim(dma_chan);
}

// Função para configurar a frequência do PWM no pino do buzzer
void set_pwm_frequency(uint gpio, uint32_t freq)
{
    uint slice_num = pwm_gpio_to_slice_num(gpio);
    // Calcula o divisor necessário para alcançar a frequência desejada com wrap de 256
    uint32_t divisor = clock_get_hz(clk_sys) / freq / 256;
    if (divisor < 16)
        divisor = 16; // Garante que o divisor não seja zero
    pwm_set_clkdiv_int_frac(slice_num, divisor / 16, divisor & 15);
}

// Função de reprodução de áudio
void play_audio()
{
    // Configura o PWM para o buzzer
    uint slice_num_A = pwm_gpio_to_slice_num(BUZZER_PIN_A);
    uint slice_num_B = pwm_gpio_to_slice_num(BUZZER_PIN_B);
    pwm_set_wrap(slice_num_A, 255); // Define o wrap (resolução do PWM)
    pwm_set_wrap(slice_num_B, 255); // Define o wrap (resolução do PWM)
    pwm_set_gpio_level(BUZZER_PIN_A, 0);
    pwm_set_gpio_level(BUZZER_PIN_B, 0);
    pwm_set_enabled(slice_num_A, true);
    pwm_set_enabled(slice_num_B, true);

    // Loop para reproduzir cada amostra do buffer
    for (int i = 0; i < BUFFER_SIZE; i++)
    {
        uint16_t sample = audio_buffer[i];
        // Mapeia o valor da amostra para uma faixa de frequência inicialmente de 2400Hz para cima
        // A variação pode ser ajustada conforme a aplicação
        uint32_t frequency = frequency_offset + ((sample * 100) / 4096);
        set_pwm_frequency(BUZZER_PIN_A, frequency);
        set_pwm_frequency(BUZZER_PIN_B, frequency);

        // Ajusta o nível do PWM para modular o volume offset
        pwm_set_gpio_level(BUZZER_PIN_A, sample + volume_offset);
        pwm_set_gpio_level(BUZZER_PIN_B, sample + volume_offset);

        sleep_us(DELAY_SAMPLE + delay_offset); // 1/SAMPLE_RATE * 1e6, delay em microsegundos do tempo das amostras, atraves do SAMPLE_RATE
    }

    // Coloca em um frequencia baixa e depois desativa o PWM ao final da reprodução
    set_pwm_frequency(BUZZER_PIN_A, 1);
    set_pwm_frequency(BUZZER_PIN_B, 1);
    pwm_set_gpio_level(BUZZER_PIN_A, 0);
    pwm_set_gpio_level(BUZZER_PIN_B, 0);
    sleep_ms(100);
    pwm_set_enabled(slice_num_A, false);
    pwm_set_enabled(slice_num_B, false);
}

// Callback para interrupção dos botões com debounce
void buttons_callback(uint gpio, uint32_t events)
{
    absolute_time_t now = get_absolute_time();

    if (gpio == BUTTON_A && (events & GPIO_IRQ_EDGE_FALL))
    {
        // Verifica debounce para o botão de gravação
        if (absolute_time_diff_us(last_button_A_press, now) / 1000 > DEBOUNCE_DELAY_MS)
        {
            system_state = STATE_RECORDING;
            last_button_A_press = now;
        }
    }

    if (gpio == BUTTON_B && (events & GPIO_IRQ_EDGE_FALL))
    {
        // Verifica debounce para o botão de reprodução
        if (absolute_time_diff_us(last_button_B_press, now) / 1000 > DEBOUNCE_DELAY_MS)
        {
            system_state = STATE_PLAYING;
            last_button_B_press = now;
        }
    }

    if (gpio == JOYSTICK_BUTTON && (events & GPIO_IRQ_EDGE_FALL))
    {
        // Verifica debounce para o botão do Joystick
        if (absolute_time_diff_us(last_button_JOYSTICK_press, now) / 1000 > DEBOUNCE_DELAY_MS)
        {
            if (system_state != STATE_MENU)
            {
                config_menu = false;
                system_state = STATE_MENU;
            }
            else
            {
                system_state = STATE_INIT;
            }
            last_button_JOYSTICK_press = now;
        }
    }
}

// Função para atualizar o display OLED com um conjunto de strings
void put_string_ssd1306(struct render_area frame_area, char *text[], int size)
{
    uint8_t ssd[ssd1306_buffer_length];
    // Zera o buffer do display
    memset(ssd, 0, ssd1306_buffer_length);
    render_on_display(ssd, &frame_area);

    int y = 0;
    for (uint i = 0; i < size; i++)
    {
        ssd1306_draw_string(ssd, 5, y, text[i], false);
        y += 8; // Incrementa a posição vertical para a próxima linha
    }
    render_on_display(ssd, &frame_area);
}

// Função para atualizar o display OLED com um conjunto de strings, invertendo as cores das linhas
void put_string_ssd1306_line_inverted(struct render_area frame_area, char *text[], int size, int lines_inverted)
{
    uint8_t ssd[ssd1306_buffer_length];
    // Zera o buffer do display
    memset(ssd, 0, ssd1306_buffer_length);
    render_on_display(ssd, &frame_area);

    int y = 0;
    bool inverted = false;
    for (uint i = 0; i < size; i++)
    {

        if (i == lines_inverted)
        {
            inverted = true;
        }
        ssd1306_draw_string(ssd, 5, y, text[i], inverted);
        inverted = false;
        y += 8; // Incrementa a posição vertical para a próxima linha
    }
    render_on_display(ssd, &frame_area);
}

int main()
{
    // Inicializa STDIO e espera conexão, se necessário
    stdio_init_all();

    // Configura os botões com pull-up e define as interrupções
    gpio_init(BUTTON_A);
    gpio_init(BUTTON_B);
    gpio_set_dir(BUTTON_A, GPIO_IN);
    gpio_set_dir(BUTTON_B, GPIO_IN);
    gpio_pull_up(BUTTON_A);
    gpio_pull_up(BUTTON_B);
    gpio_set_irq_enabled_with_callback(BUTTON_A, GPIO_IRQ_EDGE_FALL, true, &buttons_callback);
    gpio_set_irq_enabled_with_callback(BUTTON_B, GPIO_IRQ_EDGE_FALL, true, &buttons_callback);

    // Configura os controles do joystick
    adc_init();
    adc_gpio_init(JOYSTICK_Y);
    adc_gpio_init(JOYSTICK_X);
    gpio_init(JOYSTICK_BUTTON);             // Inicializa o pino do botão
    gpio_set_dir(JOYSTICK_BUTTON, GPIO_IN); // Configura o pino do botão como entrada
    gpio_pull_up(JOYSTICK_BUTTON);          // Ativa o pull-up no pino do botão para evitar flutuações
    gpio_set_irq_enabled_with_callback(JOYSTICK_BUTTON, GPIO_IRQ_EDGE_FALL, true, &buttons_callback);

    // Inicializa e configura o ADC do MIC
    adc_gpio_init(MIC_PIN);

    // Configura o FIFO do ADC:
    // - FIFO habilitado
    // - Requisição de DMA habilitada
    // - DREQ quando há 1 amostra disponível
    // - Bit de erro desabilitado
    // - **Realiza shift dos dados**, mantendo 8 bits dos 12 bits originais
    adc_fifo_setup(
        true,  // FIFO habilitado
        true,  // DMA habilitado
        1,     // DREQ com 1 amostra
        false, // Bit de erro desabilitado
        true   // Realiza shift para 8 bits (mantém 8 bits)
    );

    // Calcula o divisor do clock do ADC para atingir a taxa de amostragem desejada
    uint32_t div = clock_get_hz(clk_adc) / SAMPLE_RATE;
    adc_set_clkdiv(div);

    // Configura o pino do buzzer para função PWM
    gpio_set_function(BUZZER_PIN_A, GPIO_FUNC_PWM);
    gpio_set_function(BUZZER_PIN_B, GPIO_FUNC_PWM);

    // Inicializa o I2C para o display SSD1306
    i2c_init(I2C_PORT, ssd1306_i2c_clock * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    // Inicializa o display OLED
    ssd1306_init();
    // Define a área de renderização para o display
    struct render_area frame_area = {
        .start_column = 0,
        .end_column = ssd1306_width - 1,
        .start_page = 0,
        .end_page = ssd1306_n_pages - 1};
    calculate_render_area_buffer_length(&frame_area);

    // Textos padrões para o display
    char *text_idle[] = {
        "   Bem vindo   ",
        "      ao       ",
        " Mudaca de voz ",
        "               ",
        "Aperte o Botao ",
        " A      Gravar ",
        " B       Tocar ",
        "Joystick   Menu"};

    char *text_record[] = {
        "Comecou Gravar ",
        "               ",
        "    Aguarde    ",
        " a finalizacao ",
        "               ",
        " Gravacao e de ",
        "  5 segundos   ",
        "               "};

    char *text_play[] = {
        "Comecou a Tocar",
        "               ",
        "    Aguarde    ",
        " a finalizacao ",
        "               ",
        "Reproducao e de",
        "  5 segundos   ",
        "               "};

    // Variaveis para colocar o valor do inteiro no texto do display
    char change_frequency[16] = "";
    char change_volume[16] = "";
    char change_delay[16] = "";
    sprintf(change_frequency, "Freq     %dHz", frequency_offset);
    sprintf(change_volume, "Volume      %d", volume_offset);
    sprintf(change_delay, "Atraso    %dus", delay_offset);

    char *text_menu[] = {
        "Para Modificar ",
        "               ",
        change_frequency,
        change_volume,
        change_delay,
        "               ",
        "Voltar aperter ",
        "  no Joystick  "};

    // Variavel de atualização do texto do display
    bool update_display = false;

    // Loop principal utilizando a state machine
    while (true)
    {
        switch (system_state)
        {
        case STATE_RECORDING:
        {
            put_string_ssd1306(frame_area, text_record, count_of(text_record));
            record_audio();            // Realiza a gravação via ADC + DMA
            system_state = STATE_INIT; // Retorna ao estado inicial após gravação
            break;
        }
        case STATE_PLAYING:
        {
            put_string_ssd1306(frame_area, text_play, count_of(text_play));
            play_audio();              // Reproduz o áudio armazenado
            system_state = STATE_INIT; // Retorna ao estado inicial após reprodução
            break;
        }
        case STATE_INIT:
        {
            // Em estado inicial, exibe a tela inicial
            // Exibe mensagem inicial no OLED
            put_string_ssd1306(frame_area, text_idle, count_of(text_idle));
            system_state = STATE_IDLE; // Retorna ao estado ocioso após reprodução
            break;
        }
        case STATE_MENU:
        {
            // Coloca o texto do Menu no display
            int a = 2;
            put_string_ssd1306_line_inverted(frame_area, text_menu, count_of(text_menu), a);

            // Enquanto estiver no estado STATE_MENU, vai ficar no while
            while (system_state == STATE_MENU)
            {
                // Pegando os valores dos eixos X e Y do Joystick
                adc_select_input(JOYSTICK_Y_CHANNEL);
                uint adc_y_raw = adc_read();
                adc_select_input(JOYSTICK_X_CHANNEL);
                uint adc_x_raw = adc_read();

                // Verificar se colocou o Joystick para cima
                if (adc_y_raw == 4081)
                {
                    // Verifica se esta com a opção de configurar o menu desativado
                    if (!config_menu)
                    {
                        // Limite para não passar para as linhas acima do 2
                        if (a > 2)
                        {
                            a -= 1;
                            update_display = true;
                        }
                    }
                    // Verifica se esta com a opção de configurar o menu ativado
                    else
                    {
                        // Verifica se esta na segunda linha, que vai alterar a frequencia do PWM
                        if (a == 2)
                        {
                            frequency_offset += 100;
                            update_display = true;
                        }
                        // Verifica se esta na terceira linha, que vai alterar o duty cycle do PWM no caso volume
                        else if (a == 3)
                        {
                            if (volume_offset < 100)
                            {
                                volume_offset += 10;
                                update_display = true;
                            }
                        }
                        // Verifica se esta na quarta linha, que vai alterar no delay das amostras da reprodução do audio
                        else if (a == 4)
                        {
                            delay_offset += 5;
                            update_display = true;
                        }
                    }
                    // Verifica se atualiza o display
                    if (update_display)
                    {
                        sprintf(change_frequency, "Freq     %dHz", frequency_offset);
                        sprintf(change_volume, "Volume      %d", volume_offset);
                        sprintf(change_delay, "Atraso    %dus", delay_offset);
                        char *text_menu[] = {
                            "Para Modificar ",
                            "               ",
                            change_frequency,
                            change_volume,
                            change_delay,
                            "               ",
                            "Voltar aperter ",
                            "  no Joystick  "};
                        put_string_ssd1306_line_inverted(frame_area, text_menu, count_of(text_menu), a);
                        update_display = false;
                    }
                }
                // Verificar se colocou o Joystick para baixo
                else if (adc_y_raw == 16)
                {
                    // Verifica se esta com a opção de configurar o menu desativado
                    if (!config_menu)
                    {
                        // Limite para não passar para as linhas abaixo do 4
                        if (a < 4)
                        {
                            a += 1;
                            update_display = true;
                        }
                    }
                    // Verifica se esta com a opção de configurar o menu ativado
                    else
                    {
                        // Verifica se esta na segunda linha, que vai alterar a frequencia do PWM
                        if (a == 2)
                        {
                            frequency_offset -= 100;
                            update_display = true;
                        }
                        // Verifica se esta na terceira linha, que vai alterar o duty cycle do PWM no caso volume
                        else if (a == 3)
                        {
                            if (volume_offset > 0)
                            {
                                volume_offset -= 10;
                                update_display = true;
                            }
                        }
                        // Verifica se esta na quarta linha, que vai alterar no delay das amostras da reprodução do audio
                        else if (a == 4)
                        {
                            if (abs(delay_offset) < DELAY_SAMPLE)
                            {
                                delay_offset -= 5;
                                update_display = true;
                            }
                        }
                    }
                    // Verifica se atualiza o display
                    if (update_display)
                    {
                        sprintf(change_frequency, "Freq     %dHz", frequency_offset);
                        sprintf(change_volume, "Volume      %d", volume_offset);
                        sprintf(change_delay, "Atraso    %dus", delay_offset);
                        char *text_menu[] = {
                            "Para Modificar ",
                            "               ",
                            change_frequency,
                            change_volume,
                            change_delay,
                            "               ",
                            "Voltar aperter ",
                            "  no Joystick  "};
                        put_string_ssd1306_line_inverted(frame_area, text_menu, count_of(text_menu), a);
                        update_display = false;
                    }
                }

                // Verificar se colocou o Joystick para esquerda e desabilita alterar as variaveis de offset
                if (adc_x_raw == 16)
                {
                    config_menu = false;
                }
                // Verificar se colocou o Joystick para direita e habilita alterar as variaveis de offset
                else if (adc_x_raw == 4081)
                {
                    config_menu = true;
                }
                sleep_ms(200);
            }
            break;
        }
        case STATE_IDLE:
        default:
        {
            break;
        }
        }
        // Loop com pequeno atraso
        sleep_ms(100);
    }
    return 0;
}
