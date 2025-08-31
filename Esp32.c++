#include <Wire.h>
#include <U8g2lib.h> // Biblioteca para o display OLED
#include "Adafruit_TCS34725.h"

// =================================================================================
// Definições de Pinos para o ESP32
// =================================================================================
#define I2C_SDA_PIN 21   // Pino SDA para o display OLED (I2C_0)
#define I2C_SCL_PIN 22   // Pino SCL para o display OLED (I2C_0)
#define I2C_2_SDA_PIN 25 // Pino SDA para o sensor de cor (I2C_1)
#define I2C_2_SCL_PIN 26 // Pino SCL para o sensor de cor (I2C_1)
#define LED_PIN 4

// =================================================================================
// Definições do Display OLED (usará o barramento I2C padrão: 'Wire')
// =================================================================================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// =================================================================================
// Barramento I2C secundário para o sensor de cor
// =================================================================================
TwoWire I2C_2 = TwoWire(1);  // cria barramento I2C secundário

// =================================================================================
// Classe original tcs34725 (modificada para aceitar um barramento I2C)
// Esta classe é ótima para auto-ajuste de ganho e tempo, vamos mantê-la.
// =================================================================================
#define TCS34725_R_Coef 0.136
#define TCS34725_G_Coef 1.000
#define TCS34725_B_Coef -0.444
#define TCS34725_GA 1.0
#define TCS34725_DF 310.0
#define TCS34725_CT_Coef 3810.0
#define TCS34725_CT_Offset 1391.0

class tcs34725 {
private:
  struct tcs_agc {
    tcs34725Gain_t ag;
    uint8_t at;
    uint16_t mincnt;
    uint16_t maxcnt;
  };
  static const tcs_agc agc_lst[];
  uint16_t agc_cur;
  void setGainTime(void);
  Adafruit_TCS34725 tcs;

public:
  tcs34725(void);
  boolean begin(TwoWire *theWire = &Wire); // Modificado para aceitar um barramento
  void getData(void);
  boolean isAvailable, isSaturated;
  uint16_t againx, atime, atime_ms;
  uint16_t r, g, b, c;
  uint16_t ir;
  uint16_t r_comp, g_comp, b_comp, c_comp;
  uint16_t saturation, saturation75;
  float cratio, cpl, ct, lux, maxlux;
};

const tcs34725::tcs_agc tcs34725::agc_lst[] = {
  { TCS34725_GAIN_60X, TCS34725_INTEGRATIONTIME_614MS,     0, 20000 },
  { TCS34725_GAIN_60X, TCS34725_INTEGRATIONTIME_154MS,  4990, 63000 },
  { TCS34725_GAIN_16X, TCS34725_INTEGRATIONTIME_154MS, 16790, 63000 },
  { TCS34725_GAIN_4X,  TCS34725_INTEGRATIONTIME_154MS, 15740, 63000 },
  { TCS34725_GAIN_1X,  TCS34725_INTEGRATIONTIME_154MS, 15740, 0 }
};

tcs34725::tcs34725() : agc_cur(0), isAvailable(0), isSaturated(0) {}

boolean tcs34725::begin(TwoWire *theWire) {
  tcs = Adafruit_TCS34725(agc_lst[agc_cur].at, agc_lst[agc_cur].ag);
  if ((isAvailable = tcs.begin(TCS34725_ADDRESS, theWire))) setGainTime();
  return(isAvailable);
}

void tcs34725::setGainTime(void) {
  tcs.setGain(agc_lst[agc_cur].ag);
  tcs.setIntegrationTime(agc_lst[agc_cur].at);
  atime = int(agc_lst[agc_cur].at);
  atime_ms = ((256 - atime) * 2.4);
  switch(agc_lst[agc_cur].ag) {
    case TCS34725_GAIN_1X: againx = 1; break;
    case TCS34725_GAIN_4X: againx = 4; break;
    case TCS34725_GAIN_16X: againx = 16; break;
    case TCS34725_GAIN_60X: againx = 60; break;
  }
}

void tcs34725::getData(void) {
  tcs.getRawData(&r, &g, &b, &c);
  while(1) {
    if (agc_lst[agc_cur].maxcnt && c > agc_lst[agc_cur].maxcnt) agc_cur++;
    else if (agc_lst[agc_cur].mincnt && c < agc_lst[agc_cur].mincnt) agc_cur--;
    else break;
    setGainTime();
    delay((256 - atime) * 2.4 * 2);
    tcs.getRawData(&r, &g, &b, &c);
    break;
  }
  ir = (r + g + b > c) ? (r + g + b - c) / 2 : 0;
  r_comp = r - ir; g_comp = g - ir; b_comp = b - ir; c_comp = c - ir;
  cratio = float(ir) / float(c);
  saturation = ((256 - atime) > 63) ? 65535 : 1024 * (256 - atime);
  saturation75 = (atime_ms < 150) ? (saturation - saturation / 4) : saturation;
  isSaturated = (atime_ms < 150 && c > saturation75) ? 1 : 0;
  cpl = (atime_ms * againx) / (TCS34725_GA * TCS34725_DF);
  maxlux = 65535 / (cpl * 3);
  lux = (TCS34725_R_Coef * float(r_comp) + TCS34725_G_Coef * float(g_comp) + TCS34725_B_Coef * float(b_comp)) / cpl;
  ct = TCS34725_CT_Coef * float(b_comp) / float(r_comp) + TCS34725_CT_Offset;
}

// =================================================================================
// Bloco Principal
// =================================================================================
tcs34725 rgb_sensor;

// =================================================================================
// NOVAS FUNÇÕES DE MAPEAMENTO E CONVERSÃO DE COR (HSV)
// =================================================================================

// Estrutura para armazenar valores HSV
struct HsvColor {
    double h; // Matiz (0-360)
    double s; // Saturação (0-100)
    double v; // Valor (0-100)
};

// Função para converter RGB para HSV
HsvColor rgbToHsv(uint8_t r, uint8_t g, uint8_t b) {
    HsvColor hsv;
    double rd = (double) r / 255;
    double gd = (double) g / 255;
    double bd = (double) b / 255;
    double cmax = max(rd, max(gd, bd));
    double cmin = min(rd, min(gd, bd));
    double diff = cmax - cmin;

    // Calcula a Matiz (Hue)
    if (cmax == cmin) hsv.h = 0;
    else if (cmax == rd) hsv.h = fmod(60 * ((gd - bd) / diff) + 360, 360);
    else if (cmax == gd) hsv.h = fmod(60 * ((bd - rd) / diff) + 120, 360);
    else if (cmax == bd) hsv.h = fmod(60 * ((rd - gd) / diff) + 240, 360);

    // Calcula a Saturação (Saturation)
    if (cmax == 0) hsv.s = 0;
    else hsv.s = (diff / cmax) * 100;

    // Calcula o Valor (Value)
    hsv.v = cmax * 100;

    return hsv;
}

// Função para identificar o nome da cor com base em HSV
const char* getColorName(uint8_t r, uint8_t g, uint8_t b) {
    HsvColor hsv = rgbToHsv(r, g, b);

    // 1. Checa por Preto, Branco e Cinza primeiro
    if (hsv.v < 15) return "Preto";
    if (hsv.s < 15 && hsv.v > 85) return "Branco";
    if (hsv.s < 10) return "Cinza";

    // 2. Checa as faixas de Matiz (Hue) para as cores primárias/secundárias
    // Estes valores podem ser ajustados para melhor precisão!
    if ((hsv.h >= 0 && hsv.h <= 15) || (hsv.h >= 345 && hsv.h <= 360)) {
        return "Vermelho";
    }
    if (hsv.h >= 40 && hsv.h <= 75) {
        return "Amarelo";
    }
    if (hsv.h >= 80 && hsv.h <= 160) {
        return "Verde";
    }
    if (hsv.h >= 200 && hsv.h <= 260) {
        return "Azul";
    }
    
    // Se não se encaixar em nenhuma faixa, pode ser outra cor.
    // Pode-se adicionar mais faixas aqui (ex: Laranja, Roxo, etc.)
    return "Nao Ident.";
}

String rgbToHex(uint8_t r, uint8_t g, uint8_t b) {
    char hexColor[8];
    sprintf(hexColor, "#%02X%02X%02X", r, g, b);
    return String(hexColor);
}


void setup(void) {
  Serial.begin(115200);
  
  // Inicia o barramento I2C padrão (para o OLED)
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  
  // Inicia o barramento I2C secundário (para o sensor de cor)
  I2C_2.begin(I2C_2_SDA_PIN, I2C_2_SCL_PIN);

  // --- Inicia o OLED ---
  if (!u8g2.begin()) {
    Serial.println("Erro: Falha ao iniciar o display OLED.");
  }
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB10_tr);
  u8g2.drawStr(10, 38, "Iniciando...");
  u8g2.sendBuffer();
  delay(1500);

  // --- Inicia o Sensor de Cor no barramento I2C_2 ---
  if (rgb_sensor.begin(&I2C_2)) {
    Serial.println("Sensor TCS34725 encontrado no barramento I2C_2!");
  } else {
    Serial.println("Nenhum sensor TCS34725 encontrado... verifique as conexões.");
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(10, 38, "Erro no Sensor!");
    u8g2.sendBuffer();
    while (1);
  }
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Liga o LED para iluminar a amostra
}

void loop(void) {
  rgb_sensor.getData();

  // Garante que os valores não ultrapassem o limite de saturação do sensor
  uint16_t r_clamped = min((uint16_t)rgb_sensor.r, rgb_sensor.saturation);
  uint16_t g_clamped = min((uint16_t)rgb_sensor.g, rgb_sensor.saturation);
  uint16_t b_clamped = min((uint16_t)rgb_sensor.b, rgb_sensor.saturation);

  // Mapeia os valores lidos (0-saturação) para a escala de 8 bits (0-255)
  uint8_t r8 = map(r_clamped, 0, rgb_sensor.saturation, 0, 255);
  uint8_t g8 = map(g_clamped, 0, rgb_sensor.saturation, 0, 255);
  uint8_t b8 = map(b_clamped, 0, rgb_sensor.saturation, 0, 255);

  // *** USA A NOVA LÓGICA DE DETECÇÃO ***
  const char* colorName = getColorName(r8, g8, b8);
  String hexColor = rgbToHex(r8, g8, b8);
  
  // Converte RGB para HSV para fins de depuração
  HsvColor hsv = rgbToHsv(r8,g8,b8);

  // --- Imprime apenas o nome da cor no Monitor Serial ---
  Serial.println(colorName);

  // --- Atualiza o Display OLED ---
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_logisoso18_tr); 
  u8g2.setCursor(0, 22);
  u8g2.print(colorName);
  u8g2.drawHLine(0, 28, 128);
  u8g2.setFont(u8g2_font_t0_11b_tf); 
  u8g2.setCursor(0, 46);
  u8g2.print("Hex: ");
  u8g2.print(hexColor);
  char rgbString[20];
  sprintf(rgbString, "RGB: %d,%d,%d", r8, g8, b8);
  u8g2.setCursor(0, 62);
  u8g2.print(rgbString);
  u8g2.sendBuffer();

  delay(1000);
}

