/*
 * ============================================================
 *  Robodog Master — Arduino UNO
 *  ESP32-S3 GPIO17 (TX)  ->  Arduino D2 (RX)  [SoftwareSerial]
 *  Baud: 9600
 * 
 *  OLED Ekran Pin Bağlantıları (ESP32-S3 üzerinde):
 *  SDA  ->  ESP32-S3 GPIO 8
 *  SCL  ->  ESP32-S3 GPIO 9
 * ============================================================
 *  Komut Tablosu — FULL DUYGU MOTORU
 *  ─────────────────────────────────────────────────────────
 *  Dans:
 *    "dance_start"  →  Non-blocking dans döngüsü (müzikle)
 *    "dance_stop"   →  Dansı durdur
 *    "dance"        →  Tek seferlik kısa dans
 *
 *  Pozitif Duygular:
 *    "happy"        →  Hızlı sağ-sol wiggle + sevinç koşusu
 *    "laughing"     →  Çok hızlı wiggle + ileri atılış
 *    "excited"      →  Zigzag dans + hızlı dönüş
 *    "love"         →  Nazik sağ-sol salınım
 *    "confident"    →  Kararlı düz ileri yürüyüş
 *    "proud"        →  Yavaş şaşaalı ileri
 *    "delicious"    →  İleri-geri titreşim
 *    "relieved"     →  Yavaş nefes gibi ileri-geri
 *
 *  Negatif Duygular:
 *    "sad"          →  Yavaş geri çekilme → küsme duruşu
 *    "angry"        →  Geri çekil + saldırı
 *    "scared"       →  Panik geri + titrek
 *    "fear"         →  Hızlı kaçış
 *    "disgust"      →  Geriye iğrenerek çekil
 *    "shocked"      →  Ani geri + duraksama + ileri
 *    "embarrassed"  →  Küçük utangaç geri
 *    "bored"        →  Tembel tek sürüklenme
 *
 *  Nötr / Bilişsel Duygular:
 *    "surprised"    →  Ani geri
 *    "thinking"     →  Yavaş sağ-sol
 *    "curious"      →  Yavaş ileri prob + geri
 *    "confused"     →  Kararsız küçük sağ-sol
 *    "nervous"      →  Hızlı küçük titreme
 *    "anxious"      →  Hızlı küçük titreme
 *    "nostalgic"    →  Yavaş geri dönüş
 *    "listening"    →  Küçük ileri-geri
 *    "sleepy"       →  Çok yavaş geri
 *    "neutral"      →  Dur
 * ============================================================
 */

#include <SoftwareSerial.h>

// ── Seri port ────────────────────────────────────────────────
#define ESP_RX_PIN 2  // Arduino D2 ← ESP32-S3 TX (GPIO17)
#define ESP_TX_PIN 8  // SoftwareSerial için (kullanılmıyor)

SoftwareSerial espSerial(ESP_RX_PIN, ESP_TX_PIN);

// ── Motor A (Sol) ────────────────────────────────────────────
#define M1_AIN1 11
#define M1_AIN2 12
#define M1_PWMA 9

// ── Motor B (Sağ) ────────────────────────────────────────────
#define M2_AIN1 5
#define M2_AIN2 6
#define M2_PWMA 10

// ── Standby ─────────────────────────────────────────────────
#define STBY_PIN1 4
#define STBY_PIN2 7

// ── Hareket sabitleri ────────────────────────────────────────
#define SPD_SLOW 80
#define SPD_MED 160
#define SPD_FAST 220
#define SPD_MAX 255

// ── Dans durumu (non-blocking) ───────────────────────────────
bool dance_active = false;
unsigned long dance_step_end_ms = 0;
unsigned long dance_timeout_ms = 0;
int dance_phase = 0;

#define DANCE_STEP_MS 260
#define DANCE_SPEED 200
#define DANCE_TIMEOUT_MS 300000UL  // 5 dakika

// ── Prototip ─────────────────────────────────────────────────
void stopMotors();
void go(int d1, int d2, int s1, int s2, int t);
void wiggle(int speed, int ms, int times);
void doBehavior(const String& cmd);
void tickDance();

// ============================================================
void setup() {
    Serial.begin(9600);
    espSerial.begin(9600);

    pinMode(M1_AIN1, OUTPUT);
    pinMode(M1_AIN2, OUTPUT);
    pinMode(M1_PWMA, OUTPUT);
    pinMode(M2_AIN1, OUTPUT);
    pinMode(M2_AIN2, OUTPUT);
    pinMode(M2_PWMA, OUTPUT);
    pinMode(STBY_PIN1, OUTPUT);
    pinMode(STBY_PIN2, OUTPUT);

    digitalWrite(STBY_PIN1, HIGH);
    digitalWrite(STBY_PIN2, HIGH);
    stopMotors();
    Serial.println("[Robodog] Hazir.");
}

// ============================================================
void loop() {
    // Non-blocking dans tick
    if (dance_active)
        tickDance();

    if (!espSerial.available())
        return;

    String cmd = espSerial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0)
        return;

    Serial.print("[Komut] ");
    Serial.println(cmd);

    // Dans başlatma / durdurma
    if (cmd == "dance_start") {
        Serial.println("  -> Dans basladi");
        dance_active = true;
        dance_phase = 0;
        dance_step_end_ms = millis();
        dance_timeout_ms = millis() + DANCE_TIMEOUT_MS;
        return;
    }
    if (cmd == "dance_stop") {
        Serial.println("  -> Dans durdu");
        dance_active = false;
        stopMotors();
        return;
    }

    // Dans aktifken başka komut → dansı durdur, komutu işle
    if (dance_active) {
        dance_active = false;
        stopMotors();
    }

    doBehavior(cmd);
    stopMotors();
}

// ============================================================
//  Yardımcı: sağ-sol wiggle
// ============================================================
void wiggle(int speed, int ms, int times) {
    for (int i = 0; i < times; i++) {
        go(1, 0, speed, speed, ms);
        go(0, 1, speed, speed, ms);
    }
}

// ============================================================
//  Tüm duygu komutları
// ============================================================
void doBehavior(const String& cmd) {
    // ── DANS (tek seferlik) ──────────────────────────────────
    if (cmd == "dance") {
        for (int i = 0; i < 4; i++) {
            go(1, 0, 200, 200, 220);
            go(0, 1, 200, 200, 220);
        }
        go(1, 1, 160, 160, 380);
        go(0, 0, 160, 160, 380);
        return;
    }

    // ════════════════════════════════════════════════════════
    //  POZİTİF DUYGULAR
    // ════════════════════════════════════════════════════════

    // MUTLU — hızlı sağ-sol wiggle + sevinç koşusu
    if (cmd == "happy") {
        wiggle(SPD_FAST, 130, 6);
        go(1, 1, SPD_MED, SPD_MED, 380);
        go(0, 0, SPD_MED, SPD_MED, 280);
        go(1, 1, SPD_MED, SPD_MED, 380);
        return;
    }

    // GÜLEN — çok hızlı wiggle + küçük atılış
    if (cmd == "laughing") {
        wiggle(SPD_MAX, 90, 8);
        go(1, 1, SPD_FAST, SPD_FAST, 350);
        go(0, 0, SPD_MED, SPD_MED, 200);
        return;
    }

    // HEYECANLı — zigzag dans
    if (cmd == "excited") {
        for (int i = 0; i < 3; i++) {
            go(1, 0, SPD_FAST, SPD_FAST, 180);
            go(1, 1, SPD_FAST, SPD_FAST, 200);
            go(0, 1, SPD_FAST, SPD_FAST, 180);
            go(1, 1, SPD_FAST, SPD_FAST, 200);
        }
        return;
    }

    // SEVGİ — nazik yavaş sağ-sol
    if (cmd == "love") {
        wiggle(SPD_SLOW, 400, 3);
        go(1, 1, SPD_SLOW, SPD_SLOW, 350);
        return;
    }

    // KENDİNE GÜVEN — kararlı düz ileri yürüyüş
    if (cmd == "confident") {
        go(1, 1, SPD_FAST, SPD_FAST, 900);
        go(0, 0, SPD_MED, SPD_MED, 400);
        return;
    }

    // GURURLU — yavaş şaşaalı ileri
    if (cmd == "proud") {
        go(1, 1, SPD_SLOW + 20, SPD_SLOW + 20, 1200);
        return;
    }

    // LEZZETLİ — ileri-geri titreşim
    if (cmd == "delicious") {
        for (int i = 0; i < 4; i++) {
            go(1, 1, SPD_MED, SPD_MED, 170);
            go(0, 0, SPD_MED, SPD_MED, 170);
        }
        return;
    }

    // RAHATLADı — yavaş nefes gibi ileri-geri
    if (cmd == "relieved") {
        go(1, 1, SPD_SLOW, SPD_SLOW, 600);
        delay(200);
        go(0, 0, SPD_SLOW, SPD_SLOW, 600);
        return;
    }

    // ════════════════════════════════════════════════════════
    //  NEGATİF DUYGULAR
    // ════════════════════════════════════════════════════════

    // ÜZGÜN — yavaş geri çekilme + küsme duruşu
    if (cmd == "sad") {
        go(0, 0, SPD_SLOW + 20, SPD_SLOW + 20, 1400);
        stopMotors();
        delay(800);
        go(1, 1, SPD_SLOW, SPD_SLOW, 1000);
        return;
    }

    // KıZGIN — geri çekil + agresif ileri atılış
    if (cmd == "angry") {
        go(0, 0, SPD_FAST, SPD_FAST, 350);
        delay(100);
        go(1, 1, SPD_MAX, SPD_MAX, 450);
        go(0, 0, SPD_MED, SPD_MED, 200);
        return;
    }

    // KORKMUŞ — panik geri + titrek wiggle
    if (cmd == "scared") {
        go(0, 0, SPD_MAX, SPD_MAX, 400);
        wiggle(SPD_FAST, 100, 4);
        go(0, 0, SPD_MED, SPD_MED, 300);
        return;
    }

    // KORKU — hızlı kaçış
    if (cmd == "fear") {
        go(0, 0, SPD_MAX, SPD_MAX, 600);
        return;
    }

    // İĞRENME — geriye iğrenerek çekil
    if (cmd == "disgust") {
        go(0, 0, SPD_MED, SPD_MED, 500);
        delay(150);
        wiggle(SPD_SLOW, 200, 2);
        return;
    }

    // ŞOKLANMIŞ — ani geri + duraksama + ileri
    if (cmd == "shocked") {
        go(0, 0, SPD_MAX, SPD_MAX, 420);
        stopMotors();
        delay(500);
        go(1, 1, SPD_MED, SPD_MED, 300);
        return;
    }

    // SÜRPRİZ — ani geri
    if (cmd == "surprised") {
        go(0, 0, SPD_MAX, SPD_MAX, 420);
        return;
    }

    // UTANMIŞ — küçük utangaç geri
    if (cmd == "embarrassed") {
        go(0, 0, SPD_SLOW, SPD_SLOW, 600);
        delay(300);
        wiggle(SPD_SLOW, 200, 1);
        return;
    }

    // SIKILAN — tembel tek sürüklenme
    if (cmd == "bored") {
        go(0, 0, SPD_SLOW - 20, SPD_SLOW - 20, 1800);
        return;
    }

    // ════════════════════════════════════════════════════════
    //  NÖTR / BİLİŞSEL DUYGULAR
    // ════════════════════════════════════════════════════════

    // DÜŞÜNEN — yavaş sağ-sol
    if (cmd == "thinking") {
        go(1, 0, SPD_SLOW, SPD_SLOW, 350);
        delay(150);
        go(0, 1, SPD_SLOW, SPD_SLOW, 350);
        delay(150);
        go(1, 0, SPD_SLOW, SPD_SLOW, 350);
        return;
    }

    // MERAKLI — yavaş ileri prob + geri
    if (cmd == "curious") {
        go(1, 1, SPD_SLOW, SPD_SLOW, 500);
        delay(300);
        go(0, 0, SPD_SLOW, SPD_SLOW, 300);
        go(1, 0, SPD_SLOW, SPD_SLOW, 280);
        return;
    }

    // KAFASI KARIŞIK — kararsız küçük sağ-sol
    if (cmd == "confused") {
        for (int i = 0; i < 3; i++) {
            go(1, 0, SPD_SLOW, SPD_SLOW, 180);
            delay(80);
            go(0, 1, SPD_SLOW, SPD_SLOW, 180);
            delay(80);
        }
        return;
    }

    // GERGİN / ENDİŞELİ — hızlı küçük titreme
    if (cmd == "nervous" || cmd == "anxious") {
        for (int i = 0; i < 5; i++) {
            go(1, 0, SPD_MED, SPD_MED, 100);
            go(0, 1, SPD_MED, SPD_MED, 100);
        }
        return;
    }

    // NOSTALJİK — yavaş geri dönüş
    if (cmd == "nostalgic") {
        go(0, 0, SPD_SLOW, SPD_SLOW, 1000);
        delay(400);
        go(1, 1, SPD_SLOW, SPD_SLOW, 800);
        return;
    }

    // DİNLİYOR — küçük ileri-geri
    if (cmd == "listening") {
        go(1, 1, SPD_SLOW, SPD_SLOW, 200);
        go(0, 0, SPD_SLOW, SPD_SLOW, 200);
        return;
    }

    // UYKULU — çok yavaş geri
    if (cmd == "sleepy") {
        go(0, 0, SPD_SLOW - 20, SPD_SLOW - 20, 1600);
        return;
    }

    // NÖTR — dur (hiç hareket yok)
    if (cmd == "neutral") {
        return;
    }

    // ── Bilinmeyen komut → kısa titreşim ────────────────────
    wiggle(SPD_SLOW + 20, 160, 2);
}

// ============================================================
//  Non-blocking dans adımı
//  Phase 0: sağa | 1: sola | 2: ileri | 3: geri
// ============================================================
void tickDance() {
    unsigned long now = millis();

    // 5 dakika timeout — kimse durdurmasa otomatik dur
    if (now >= dance_timeout_ms) {
        dance_active = false;
        stopMotors();
        Serial.println("  -> Dans timeout (5dk)");
        return;
    }

    if (now < dance_step_end_ms)
        return;

    dance_phase = (dance_phase + 1) % 4;

    switch (dance_phase) {
        case 0:  // Sağa dön
            digitalWrite(M1_AIN1, HIGH);
            digitalWrite(M1_AIN2, LOW);
            digitalWrite(M2_AIN1, LOW);
            digitalWrite(M2_AIN2, HIGH);
            analogWrite(M1_PWMA, DANCE_SPEED);
            analogWrite(M2_PWMA, DANCE_SPEED);
            break;
        case 1:  // Sola dön
            digitalWrite(M1_AIN1, LOW);
            digitalWrite(M1_AIN2, HIGH);
            digitalWrite(M2_AIN1, HIGH);
            digitalWrite(M2_AIN2, LOW);
            analogWrite(M1_PWMA, DANCE_SPEED);
            analogWrite(M2_PWMA, DANCE_SPEED);
            break;
        case 2:  // İleri
            digitalWrite(M1_AIN1, HIGH);
            digitalWrite(M1_AIN2, LOW);
            digitalWrite(M2_AIN1, HIGH);
            digitalWrite(M2_AIN2, LOW);
            analogWrite(M1_PWMA, DANCE_SPEED - 30);
            analogWrite(M2_PWMA, DANCE_SPEED - 30);
            break;
        case 3:  // Geri
            digitalWrite(M1_AIN1, LOW);
            digitalWrite(M1_AIN2, HIGH);
            digitalWrite(M2_AIN1, LOW);
            digitalWrite(M2_AIN2, HIGH);
            analogWrite(M1_PWMA, DANCE_SPEED - 30);
            analogWrite(M2_PWMA, DANCE_SPEED - 30);
            break;
    }
    dance_step_end_ms = now + DANCE_STEP_MS;
}

// ============================================================
//  Motor sürüşü — soft-start
// ============================================================
void go(int d1, int d2, int s1, int s2, int t) {
    digitalWrite(M1_AIN1, d1 ? HIGH : LOW);
    digitalWrite(M1_AIN2, d1 ? LOW : HIGH);
    digitalWrite(M2_AIN1, d2 ? HIGH : LOW);
    digitalWrite(M2_AIN2, d2 ? LOW : HIGH);
    for (int s = 0; s <= 100; s += 25) {
        analogWrite(M1_PWMA, min(s, s1));
        analogWrite(M2_PWMA, min(s, s2));
        delay(4);
    }
    analogWrite(M1_PWMA, s1);
    analogWrite(M2_PWMA, s2);
    delay(t);
}

// ============================================================
void stopMotors() {
    analogWrite(M1_PWMA, 0);
    analogWrite(M2_PWMA, 0);
}
