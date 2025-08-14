#include <M5Unified.h>
#include <SD.h>

M5Canvas canvas(&M5.Display);  // sprite

// Mitutoyo SPC(Digimatic)接続ピン (M5Stack Basic)
int req = 26;  // REQ (データ要求) 出力
int dat = 17;  // DATA 入力
int clk = 16;  // CLK  入力

// 外部スイッチ等のアナログ監視（そのまま）
const int CH2_PIN = 36;
float value2 = 0;

// ---- helper: 指定状態の間待つ(µsタイムアウト) ----
static inline bool waitWhileStateTimeout(int pin, int state, uint32_t timeout_us) {
  uint32_t t0 = micros();
  while (digitalRead(pin) == state) {
    if ((uint32_t)(micros() - t0) > timeout_us) return false;
  }
  return true;
}

// 画面右上にバッテリー％表示
static inline void drawBatteryTopRight(M5Canvas& g, int lvl) {
  char buf[8];
  if (lvl >= 0) snprintf(buf, sizeof(buf), "%3d%%", lvl);
  else          snprintf(buf, sizeof(buf), "EXT");
  g.setTextColor(TFT_WHITE, TFT_BLACK);
  g.setTextDatum(textdatum_t::top_right);
  g.setTextSize(3);
  g.drawString(buf, g.width() - 2, 2);
  g.setTextDatum(textdatum_t::top_left);
}

// 未接続画面を表示（中央NoDvice＋右上バッテリー）
static inline void showNoDeviceScreen(M5Canvas& g, int batteryNow) {
  g.fillScreen(TFT_BLACK);
  drawBatteryTopRight(g, batteryNow);
  g.setTextColor(TFT_WHITE, TFT_BLACK);
  g.setTextSize(6);
  g.setTextDatum(textdatum_t::middle_center);
  g.drawString("NoDvice", g.width()/2, g.height()/2);
  g.setTextDatum(textdatum_t::top_left);
  g.pushSprite(0, 0);
}

int i = 0, j = 0, k = 0;
int sign = 0;
int decimal;
float dpp;

int units;
byte mydata[14];
String value_str;
long value_int;
float value;
float Sum_value = 0;
float Avr_value = 0;

File csvFile;
int cnt = 1;

unsigned long btnAPressTime = 0;
bool btnALongPressHandled = false;

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true;
  M5.begin(cfg);
  M5.Power.begin();

  pinMode(req, OUTPUT);
  pinMode(clk, INPUT_PULLUP);
  pinMode(dat, INPUT_PULLUP);
  pinMode(CH2_PIN, INPUT);
  digitalWrite(req, LOW);

  canvas.setColorDepth(8);
  canvas.setTextSize(3);
  canvas.createSprite(M5.Display.width(), M5.Display.height());

  // SD初期化（失敗しても再試行）
  while (false == SD.begin(GPIO_NUM_4, SPI, 25000000)) {
    delay(500);
  }
}

void loop() {
  M5.update();

  int batteryNow = M5.Power.getBatteryLevel();

  // BtnA 1秒長押しで nashi.csv の末行削除（元ロジック踏襲）
  if (M5.BtnA.isPressed()) {
    if (btnAPressTime == 0) btnAPressTime = millis();
    if ((millis() - btnAPressTime > 1000) && !btnALongPressHandled) {
      M5.Speaker.setVolume(100);
      M5.Speaker.tone(440, 300); delay(300); M5.Speaker.end(); delay(100);
      M5.Speaker.begin(); M5.Speaker.tone(440, 300); delay(300); M5.Speaker.end(); delay(100);
      M5.Speaker.begin(); M5.Speaker.tone(440, 100); M5.Speaker.end();

      // nashi.csv の末行削除
      File inputFile = SD.open("/nashi.csv", FILE_READ);
      if (inputFile) {
        String lines[100];  // 最大100行（必要に応じて拡張）
        int lineCount = 0;
        while (inputFile.available() && lineCount < 100) {
          String line = inputFile.readStringUntil('\n');
          line.trim();
          if (line.length() > 0) lines[lineCount++] = line;
        }
        inputFile.close();

        File outputFile = SD.open("/test.csv", FILE_WRITE);
        if (outputFile) {
          for (int i = 0; i < lineCount - 1; i++) outputFile.println(lines[i]);
          outputFile.close();
          SD.remove("/nashi.csv");
          SD.rename("/test.csv", "/nashi.csv");
        }
      }

      cnt = max(1, cnt - 1);
      canvas.fillScreen(BLACK);
      canvas.pushSprite(0, 0);
      btnALongPressHandled = true;
    }
  } else {
    btnAPressTime = 0;
    btnALongPressHandled = false;
  }

  value2 = analogRead(CH2_PIN);   // 外部入力（そのまま）
  // Serial.println(value2);

  
// ---- Mitutoyo SPCの1フレーム取得（オートアライン版） ----
digitalWrite(req, HIGH);

// 13ニブル×4bitを取得（各ハーフサイクル 100ms タイムアウト）
byte raw[13];
bool ok = true;
for (i = 0; i < 13; i++) {
  k = 0;
  for (j = 0; j < 4; j++) {
    if (!waitWhileStateTimeout(clk, LOW,  100000)) { ok = false; break; }
    if (!waitWhileStateTimeout(clk, HIGH, 100000)) { ok = false; break; }
    bitWrite(k, j, (digitalRead(dat) & 0x1));
  }
  if (!ok) break;
  raw[i] = k;
}

if (!ok) {
  digitalWrite(req, LOW);
  showNoDeviceScreen(canvas, batteryNow);
  delay(50);
  return;
}

// --- オートアライン ---
// 0..12シフトを試し、以下の条件を満たすアラインを採用
//  - sign(=nib[4]) が {0,8}
//  - decimal(=nib[11]) が 0..3
//  - digits(=nib[5..10]) が全て 0..9
byte aligned[13];
int bestShift = -1;
for (int s = 0; s < 13; s++) {
  for (int t = 0; t < 13; t++) aligned[t] = raw[(t + s) % 13];

  int sign_cand = aligned[4];
  int dec_cand  = aligned[11];
  bool digits_ok = true;
  for (int d = 5; d <= 10; d++) {
    if (aligned[d] > 9) { digits_ok = false; break; }
  }
  bool cond = ((sign_cand == 0 || sign_cand == 8) && (dec_cand >= 0 && dec_cand <= 3) && digits_ok);
  if (cond) { bestShift = s; break; }
}
if (bestShift >= 0) {
  for (int t = 0; t < 13; t++) mydata[t] = aligned[t];
} else {
  // 条件に合致しなければ未接続扱い
  digitalWrite(req, LOW);
  showNoDeviceScreen(canvas, batteryNow);
  delay(50);
  return;
}

// --- デコード ---
sign = mydata[4];
value_str = String(mydata[5]) + String(mydata[6]) + String(mydata[7]) +
            String(mydata[8]) + String(mydata[9]) + String(mydata[10]);
decimal = mydata[11];
units   = mydata[12];


  value_int = value_str.toInt();
  dpp      = pow(10.0, decimal);
  value    = value_int / dpp;
  // Serial.println(value);

  // ---- 表示（接続時） ----
  int BatteryLevel = batteryNow;  // 右上表示に利用

  canvas.fillScreen(BLACK);
  if (sign == 0) {
    canvas.setCursor(10, 20);
    canvas.setTextSize(3);
    canvas.printf("count:");
    canvas.setCursor(140, 20);
    canvas.printf("%d", cnt);

    // 右上にバッテリー
    canvas.setCursor(230, 20);
    canvas.printf("%3d%%", BatteryLevel);

    canvas.setCursor(80, 100);
    canvas.setTextSize(6);
    canvas.printf("%4.2f\n", value);
    canvas.setCursor(60, 170);
    canvas.setTextSize(4);
    canvas.printf("Avr:");
    canvas.setCursor(160, 170);
    canvas.printf("%4.2f\n", Avr_value);
  } else if (sign == 8) {
    canvas.setCursor(80, 90);
    canvas.setTextSize(6);
    canvas.printf("-%4.2f\n", value);
  }
  canvas.pushSprite(0, 0);

  digitalWrite(req, LOW);

  // ---- SD記録（外部入力が0のとき）----
  if (value2 == 0) {
    csvFile = SD.open("/nashi.csv", FILE_APPEND);
    if (csvFile) {
      canvas.fillScreen(BLACK);
      canvas.setCursor(130, 80);
      canvas.setTextSize(6);
      canvas.print("OK");
      canvas.pushSprite(0, 0);

      M5.Speaker.setVolume(100);
      M5.Speaker.tone(2637, 300); delay(300); M5.Speaker.end(); delay(30);

      csvFile.print(cnt);
      csvFile.print(",");
      csvFile.println(value);
      Sum_value += value;
      Avr_value = Sum_value / cnt;
      cnt++;
      csvFile.close();
    } else {
      canvas.fillScreen(BLACK);
      canvas.setCursor(155, 80);
      canvas.setTextSize(6);
      canvas.print("X");
      canvas.pushSprite(0, 0);
      M5.Speaker.setVolume(100);
      M5.Speaker.tone(500, 300); delay(300); M5.Speaker.end();
    }
    delay(50);
  }

  delay(100);
}
