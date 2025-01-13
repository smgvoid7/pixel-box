#include <ModbusMaster.h>  // ModbusMaster kütüphanesi

ModbusMaster nodeMeter;      // Modbus Master nesnesi (meter için)
ModbusMaster nodeInverter;   // Modbus Master nesnesi (inverter için)

// RS485 pin tanımlamaları (farklı seri portlar kullanarak)
#define RS485_RX_METER_PIN 16      // UART2 RX pini (GPIO16) - Meter
#define RS485_TX_METER_PIN 17      // UART2 TX pini (GPIO17) - Meter
#define RS485_RX_INVERTER_PIN 18   // UART1 RX pini (GPIO18) - Inverter
#define RS485_TX_INVERTER_PIN 19   // UART1 TX pini (GPIO19) - Inverter

// LED pin tanımlamaları
#define GREEN_LED_PIN 13     // Yeşil LED
#define RED_LED_PIN 15       // Kırmızı LED

// Register adresleri
const uint16_t totalActivePowerRegister = 0x0026; // Meter total aktif güç register adresi (201AH)
const uint16_t activePowerRegister = 4010;        // Inverter aktif güç ayar register adresi (4010)

// Slave adresleri
const uint8_t meterSlaveAddress = 2;       // Meter için slave adresi
const uint8_t inverterSlaveAddress = 1;    // Inverter için slave adresi

uint8_t demantCounter = 15;
uint8_t demantValue = 0;

float lastInverterPowerSent = -1; // Son gönderilen güç değeri
bool isMeterReadingActive = true; // Meter okuma döngüsünü kontrol eden bayrak

void setup() {
  Serial.begin(9600);

  // Modbus cihazlarını başlat (farklı seri portlar ve baud rate'ler ile)
  Serial2.begin(115200, SERIAL_8N1, RS485_RX_METER_PIN, RS485_TX_METER_PIN); // Meter için 4800 bps
  nodeMeter.begin(meterSlaveAddress, Serial2);

  Serial1.begin(9600, SERIAL_8N1, RS485_RX_INVERTER_PIN, RS485_TX_INVERTER_PIN); // Inverter için 9600 bps
  nodeInverter.begin(inverterSlaveAddress, Serial1);

  // LED pinlerini çıkış olarak ayarla
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);

  Serial.println("Meter ve inverter için Modbus RS485 haberleşmesi başlatıldı.");
}

void loop() {
  // Eğer meter okuma döngüsü aktifse
  if (isMeterReadingActive) {
    // Sürekli 30 okuma yaparak ortalama güç hesapla
    float powerLimit = calculateAveragePower(60);

    int realPowerLimit = 0;
        
    if((int)round(powerLimit) > powerLimit){
      realPowerLimit = (int)round(powerLimit) - 1;
    }
    else{
      realPowerLimit = (int)round(powerLimit);
    }

    if(demantCounter > 0){
      demantValue += realPowerLimit;
      demantCounter--;
    }
    else{
      Serial.println("Demant değeri: " + String(demantValue / 15));
      demantValue = 0;    
      demantCounter = 15;
    }

    // Eğer ortalama güç limiti geçerliyse ve invertere son gönderilen güçle ±%5 fark varsa güncelle
    if (powerLimit >= 0) {
      if (powerLimit >= 88) {
        powerLimit = 88;  // Inverter'in maksimum sınırı
      }

      // ±%5 fark kontrolü: hem %5'ten büyük hem de %5'ten küçük farklar için
      float percentDifference = ((powerLimit - lastInverterPowerSent) / lastInverterPowerSent) * 100;
      if (lastInverterPowerSent == -1 || percentDifference > 5 || percentDifference < -5) {
        // Invertere veri yazma sırasında meter okumasını devre dışı bırak
        isMeterReadingActive = false;

        Serial.print("Inverter'e ayarlanacak yeni güç sınırı: ");
        Serial.println(realPowerLimit);

        if (attemptWriteInverterActivePower(realPowerLimit)) {
          lastInverterPowerSent = powerLimit; // Başarılı yazmada son gönderilen değeri güncelle
          digitalWrite(GREEN_LED_PIN, HIGH); 
          digitalWrite(RED_LED_PIN, LOW);    
        } else {
          Serial.println("Inverter'e veri yazılamadı. Kırmızı LED yanıyor.");
          digitalWrite(RED_LED_PIN, HIGH);   
          delay(5000);
          digitalWrite(RED_LED_PIN, LOW);
        }

        // Invertere veri yazıldıktan sonra meter okumasını tekrar etkinleştir
        delay(1000);  // Hattın stabilize olması için kısa bir gecikme
        isMeterReadingActive = true;
      } else {
        Serial.println("Son gönderilen değerden ±%5 fark yok, invertere yeni veri gönderilmedi.");
      }
    } else {
      Serial.println("Meter verisi okunamadı, invertere yazılmadı.");
      digitalWrite(GREEN_LED_PIN, LOW); 
      delay(200);
      digitalWrite(RED_LED_PIN, HIGH);  
    }
  }

  delay(200); // her döngü için bekleme süresi
}


// 30 kez başarılı veri okuma sonrası ortalama güç hesaplama fonksiyonu
float calculateAveragePower(int targetReadCount) {
  float totalPower = 0;
  int successfulReads = 0;

  while (successfulReads < targetReadCount) {
    if (!isMeterReadingActive) return -1;

    float power = readTotalActivePower();
    
    if (power >= 0) {
      totalPower += power;
      successfulReads++;

      digitalWrite(GREEN_LED_PIN, HIGH);
      delay(200);                        
      digitalWrite(GREEN_LED_PIN, LOW);
      delay(200);
    } else {
      digitalWrite(RED_LED_PIN, HIGH);
      delay(200);
      digitalWrite(RED_LED_PIN, LOW);
      delay(200);
    }

    delay(200);
  }

  float averagePower = totalPower / targetReadCount;
  Serial.print("Ortalama güç (tüketim): ");
  Serial.print(averagePower);
  Serial.println(" kW");
  return averagePower;
}

float readTotalActivePower() {
  nodeMeter.clearResponseBuffer();

  uint8_t result = nodeMeter.readHoldingRegisters(totalActivePowerRegister, 2);
  if (result == nodeMeter.ku8MBSuccess) {
    uint32_t combinedValue = ((uint32_t)nodeMeter.getResponseBuffer(0) << 16) | nodeMeter.getResponseBuffer(1);
    float activePower = *(float*)&combinedValue;

    activePower = activePower / 1000;

    activePower = activePower - (activePower * (10/100));

    //activePower = 2;

    if (activePower < 0) activePower = -activePower;

    Serial.print("Meter'dan okunan toplam aktif güç (tüketim): ");
    Serial.print(activePower);
    Serial.println(" kW");
    return activePower;
  } else {
    Serial.print("Meter okuma hatası, Hata Kodu: ");
    Serial.println(result);

    return 0;
  }
}

bool attemptWriteInverterActivePower(int powerLimit) {
  int maxRetries = 3;
  int attempt = 0;
  uint8_t result;

  while (attempt < maxRetries) {
    nodeInverter.clearResponseBuffer();

    result = nodeInverter.writeSingleRegister(activePowerRegister, powerLimit);

    if (result == nodeInverter.ku8MBSuccess) {
      Serial.print("Inverter'e ");
      Serial.print(powerLimit);
      Serial.println(" kW güç sınırı başarıyla yazıldı.");
      return true;
    } else {
      attempt++;
      Serial.print("Inverter yazma hatası, Hata Kodu: ");
      Serial.print(result);
      Serial.print(". Yeniden deneme: ");
      Serial.println(attempt);
      delay(1000); 
    }
  }

  Serial.println("Inverter yazma başarısız. Maksimum tekrar sayısına ulaşıldı.");
  return false;
}
