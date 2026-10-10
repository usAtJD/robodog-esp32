# ESP32-S3 için 7-Pin SPI OLED Türkçe Pin Bağlantı Rehberi

## 📌 ESP32-S3 → 7-Pin SPI OLED Ekran Bağlantıları

```
ESP32-S3 Kartı                    7-Pin SPI OLED Ekran
┌─────────────┐                  ┌─────────────────┐
│             │                  │                 │
│ GPIO 11     ├──────────────────→│ SDA (MOSI)     │
│             │                  │                 │
│ GPIO 12     ├──────────────────→│ SCK (Saat)     │
│             │                  │                 │
│ GPIO 10     ├──────────────────→│ CS (Seçim)     │
│             │                  │                 │
│ GPIO 13     ├──────────────────→│ DC (Kontrol)   │
│             │                  │                 │
│ GPIO 14     ├──────────────────→│ RES (Sıfırla)  │
│             │                  │                 │
│ 3.3V        ├──────────────────→│ VCC (Güç)      │
│             │                  │                 │
│ GND         ├──────────────────→│ GND (Toprak)   │
│             │                  │                 │
└─────────────┘                  └─────────────────┘
```

## 🔌 Detaylı Bağlantı Tablosu

| ESP32-S3 Pin | OLED Pin | Fonksiyon | Açıklama |
|-------------|----------|-----------|----------|
| **GPIO 11**  | **SDA**  | MOSI Veri | ESP32-S3 → OLED veri hattı |
| **GPIO 12**  | **SCK**  | Saat Sinyali | SPI iletişim için clock |
| **GPIO 10**  | **CS**   | Chip Select | OLED ekranını seçer |
| **GPIO 13**  | **DC**   | Data/Command | Komut mu veri mi ayırır |
| **GPIO 14**  | **RES**  | Reset | Ekranı sıfırlar |
| **3.3V**     | **VCC**  | Güç | 3.3V güç kaynağı |
| **GND**      | **GND**  | Toprak | Toprak bağlantısı |

## ⚠️ ÇOK ÖNEMLİ UYARILAR

### ❌ KESİNLİKLE YAPILMAYACAKLAR:
- **5V BAĞLAMAYIN!** - OLED ekranı kalıcı olarak bozar
- **Pinleri ters bağlamayın** - Çalışmaz, zarar verebilir
- **GND bağlantısını atlamayın** - Kararsız çalışma
- **3.3V yerine 5V kullanmayın** - Ekran yanar

### ✅ MUTLAKA YAPILACAKLAR:
- Tüm bağlantıları güç vermeden önce kontrol edin
- Kısa ve kaliteli kablolar kullanın
- Bağlantıları sıkıca tutturun
- Multimetre ile continuity testi yapın (mümkünse)

## 🔧 Adım Adım Kurulum

### 1. Adım: Güç Bağlantıları (ÖNCE BU!)
```
ESP32-S3 3.3V → OLED VCC
ESP32-S3 GND  → OLED GND
```

### 2. Adım: SPI Veri Bağlantıları
```
ESP32-S3 GPIO 11 → OLED SDA (MOSI)
ESP32-S3 GPIO 12 → OLED SCK (Clock)
```

### 3. Adım: Kontrol Bağlantıları
```
ESP32-S3 GPIO 10 → OLED CS (Chip Select)
ESP32-S3 GPIO 13 → OLED DC (Data/Command)
ESP32-S3 GPIO 14 → OLED RES (Reset)
```

## 📋 Kontrol Listesi

### Bağlantı Kontrolü:
- [ ] GPIO 11 → SDA (MOSI Veri)
- [ ] GPIO 12 → SCK (Clock)
- [ ] GPIO 10 → CS (Chip Select)
- [ ] GPIO 13 → DC (Data/Command)
- [ ] GPIO 14 → RES (Reset)
- [ ] 3.3V → VCC (Güç)
- [ ] GND → GND (Toprak)

### Güvenlik Kontrolü:
- [ ] Sadece 3.3V kullanıyorum (5V YOK!)
- [ ] Tüm pinler doğru bağlandı
- [ ] Kısa devre yok
- [ ] Bağlantılar sağlam

## 🚀 Yazılım Kurulumu

### 1. Mevcut Dosyayı Yedekle
```bash
cp esp32s3_korvo2_v3_board.cc esp32s3_korvo2_v3_board_yedek.cc
```

### 2. SPI OLED Dosyasını Kullan
```bash
# Yeni SPI OLED dosyasını mevcut dosyanın yerine kopyala
cp esp32s3_korvo2_v3_board_spi_oled.cc esp32s3_korvo2_v3_board.cc
```

### 3. Derleme ve Flash
```bash
idf.py build
idf.py flash monitor
```

## 📱 Beklenen Serial Monitör Çıktısı

SPI OLED çalıştığında şunu görmelisiniz:
```
I (xxx) esp32s3_korvo2_v3_spi_oled: Initializing 7-pin SPI OLED display for ESP32-S3
I (xxx) esp32s3_korvo2_v3_spi_oled: Install SSD1306 SPI driver
I (xxx) esp32s3_korvo2_v3_spi_oled: SSD1306 SPI driver installed
I (xxx) esp32s3_korvo2_v3_spi_oled: 7-pin SPI OLED display initialized successfully
```

## 🔧 Sorun Giderme

### Ekran Hiç Çalışmıyorsa:
1. **Güç bağlantısını kontrol et** (3.3V mi?)
2. **GND bağlantısını kontrol et**
3. **Tüm pinlerin doğru bağlı olduğundan emin ol**
4. **ESP32-S3'i yeniden başlat**

### Ekran Karanlık ise:
1. **VCC bağlantısını kontrol et** (3.3V gidiyor mu?)
2. **GND bağlantısını kontrol et**
3. **Ekranı değiştir** (bozuk olabilir)

### Garip Karakterler veya Çizimler varsa:
1. **GPIO 11 ve 12 pinlerini kontrol et** (SDA/SCK)
2. **GPIO 13 pinini kontrol et** (DC)
3. **SPI frekansını düşür** (kodda 20MHz)

### Ekran Titriyor veya Kararsız ise:
1. **Güç bağlantısını kontrol et** (3.3V stabil mi?)
2. **GND bağlantısını kontrol et**
3. **Kabloları kısalt**
4. **0.1μF kondansatör ekle** (VCC-GND arası)

## 🎯 ESP32-S3 Özel Notları

### Neden Bu Pinler?
- **GPIO 11-14**: ESP32-S3'ün SPI3 pinleri
- **SPI3**: Diğer SPI cihazlarıyla çakışmaz
- **20MHz**: ESP32-S3 için optimal hız
- **DMA**: Verimli veri transferi

### Diğer Pin Seçenekleri:
Eğer bu pinler meşgulse:
- **GPIO 35, 36, 37, 38** (SPI2 alternatif)
- **GPIO 6, 7, 8, 9** (SPI2 alternatif)

## 📞 Yardım İçin

Sorun yaşarsanız:
1. **Bağlantı fotoğrafını çekin**
2. **Serial monitör çıktısını paylaşın**
3. **Hangi adımda sorun olduğunu belirtin**
4. **ESP32-S3 kart modelinizi söyleyin**

**Başarılı kurulum! 🎉 OLED ekranınız ESP32-S3 ile çalışacak!**
