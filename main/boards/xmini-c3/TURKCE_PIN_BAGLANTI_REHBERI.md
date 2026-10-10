# 7-Pin SPI OLED Türkçe Pin Bağlantı Rehberi

## 📌 Pin Bağlantıları

### ESP32-C3 → 7-Pin SPI OLED Ekran

```
ESP32-C3 Kartı                    7-Pin SPI OLED Ekran
┌─────────────┐                  ┌─────────────────┐
│             │                  │                 │
│ GPIO 18     ├──────────────────→│ SCK (Saat)     │
│             │                  │                 │
│ GPIO 23     ├──────────────────→│ SDA (Veri)     │
│             │                  │                 │
│ GPIO 5      ├──────────────────→│ CS (Seçim)     │
│             │                  │                 │
│ GPIO 4      ├──────────────────→│ DC (Kontrol)   │
│             │                  │                 │
│ GPIO 2      ├──────────────────→│ RES (Sıfırla)  │
│             │                  │                 │
│ 3.3V        ├──────────────────→│ VCC (Güç)      │
│             │                  │                 │
│ GND         ├──────────────────→│ GND (Toprak)   │
│             │                  │                 │
└─────────────┘                  └─────────────────┘
```

## 🔌 Detaylı Bağlantı Tablosu

| ESP32-C3 Pin | OLED Pin | Fonksiyon | Açıklama |
|-------------|----------|-----------|----------|
| **GPIO 18**  | **SCK**  | Saat Sinyali | SPI iletişim için clock sinyali |
| **GPIO 23**  | **SDA**  | Veri Hattı | MOSI veri hattı (ESP32 → OLED) |
| **GPIO 5**   | **CS**   | Chip Select | OLED ekranını seçer |
| **GPIO 4**   | **DC**   | Data/Command | Komut mu veri mi ayırır |
| **GPIO 2**   | **RES**  | Reset | Ekranı sıfırlar |
| **3.3V**     | **VCC**  | Güç | 3.3V güç kaynağı |
| **GND**      | **GND**  | Toprak | Toprak bağlantısı |

## ⚠️ ÖNEMLİ UYARILAR

### ❌ YAPILMAYACAKLAR:
- **5V BAĞLAMAYIN!** - OLED ekranı bozar
- Pinleri ters bağlamayın
- GND bağlantısını unutmayın
- 3.3V yerine 5V kullanmayın

### ✅ YAPILACAKLAR:
- Tüm bağlantıları kontrol edin
- Kısa kablo kullanın
- Bağlantıları sabitleyin
- Güç kesin bağlantı yapın

## 🔧 Adım Adım Kurulum

### 1. Adım: Güç Bağlantıları
```
ESP32-C3 3.3V → OLED VCC
ESP32-C3 GND  → OLED GND
```

### 2. Adım: SPI Bağlantıları
```
ESP32-C3 GPIO 18 → OLED SCK
ESP32-C3 GPIO 23 → OLED SDA
```

### 3. Adım: Kontrol Bağlantıları
```
ESP32-C3 GPIO 5  → OLED CS
ESP32-C3 GPIO 4  → OLED DC
ESP32-C3 GPIO 2  → OLED RES
```

## 📋 Kontrol Listesi

### Bağlantı Kontrolü:
- [ ] GPIO 18 → SCK (Clock)
- [ ] GPIO 23 → SDA (MOSI Veri)
- [ ] GPIO 5  → CS (Chip Select)
- [ ] GPIO 4  → DC (Data/Command)
- [ ] GPIO 2  → RES (Reset)
- [ ] 3.3V    → VCC (Güç)
- [ ] GND     → GND (Toprak)

### Güvenlik Kontrolü:
- [ ] 5V kullanmıyorum (sadece 3.3V)
- [ ] Tüm pinler doğru bağlandı
- [ ] Kısa devre yok
- [ ] Bağlantılar sağlam

## 🚀 Yazılım Kurulumu

### 1. Mevcut Dosyayı Yedekle
```bash
cp xmini_c3_board.cc xmini_c3_board_yedek.cc
```

### 2. SPI Versiyonunu Kullan
```bash
# Yeni SPI dosyasını mevcut dosyanın yerine kopyala
cp xmini_c3_board_spi.cc xmini_c3_board.cc
```

### 3. Derleme ve Flash
```bash
idf.py build
idf.py flash monitor
```

## 📱 Beklenen Çıktı

SPI OLED çalıştığında serial monitörde şunu görmelisiniz:
```
I (xxx) XminiC3BoardSPI: Initializing 7-pin SPI OLED display
I (xxx) XminiC3BoardSPI: Install SSD1306 SPI driver
I (xxx) XminiC3BoardSPI: SSD1306 SPI driver installed
I (xxx) XminiC3BoardSPI: 7-pin SPI OLED display initialized successfully
```

## 🔧 Sorun Giderme

### Ekran Çalışmıyorsa:
1. **Güç bağlantısını kontrol et** (3.3V mi?)
2. **GND bağlantısını kontrol et**
3. **Pinlerin doğru bağlı olduğundan emin ol**
4. **Ekranı sıfırla** (RES pinini kontrol et)

### Ekran Karanlık ise:
1. **VCC bağlantısını kontrol et**
2. **3.3V sağlandığından emin ol**
3. **GND bağlantısını kontrol et**

### Garip Görüntü varsa:
1. **SCK ve SDA pinlerini kontrol et**
2. **DC pin bağlantısını kontrol et**
3. **SPI frekansını düşür** (kodda 20MHz)

## 🎯 Performans Avantajları

- **~50x daha hızlı** iletişim (20MHz vs 400kHz)
- **Daha akıcı animasyonlar**
- **Daha iyi yanıt süresi**
- **Daha stabil bağlantı**

## 📞 Yardım

Eğer sorun yaşarsanız:
1. Bağlantıları tekrar kontrol edin
2. Serial monitör çıktısını paylaşın
3. Hangi adımda sorun olduğunu belirtin

**Başarılar! 🎉**
