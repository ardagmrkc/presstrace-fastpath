# AI Kullanım Notu

> **Taslak:** Aşağıdaki maddeler çalışma oturumunda gerçekten yapılanların kaydıdır. Teslim
> öncesi kendi ifadenizle gözden geçirip düzenleyin (şartname: "açıklama ve doğrulama size ait").

Araç: Claude (Anthropic), Claude Code üzerinden.

## Hangi işlerde destek alındı?

- Proje iskeleti ve dizin yapısı. IAR projesi, IAR 9.70.4'ün ürettiği boş projeden türetildi.
- Firmware:
  - Register seviyesi sürücüler: RCC, GPIO, EXTI, TIM2, USART2, ADC.
  - Üç görev, kuyruklar, buton ISR'si ve tekrar-kenar filtresi.
  - Kayıt havuzu, UART TC tabanlı t₄ ölçümü, ASCII 64 baytlık mesaj kodlayıcısı.
  - Senaryo komutu, kart sıcaklığı telemetrisi.
- FreeRTOS-Kernel'in git alt modülü olarak eklenmesi, `FreeRTOSConfig.h`, CMSIS dosyalarının
  seçilmesi.
- Web Serial arayüzü, `analyze.py`, README ve `docs/` belgeleri.

## Üretilen kod nasıl kontrol edildi?

- **Derleme:** Firmware IAR 9.70.4 komut satırı derleyicisiyle derlendi ve uyarılar sıfırlandı.
  İlk derlemedeki 4 uyarıdan ikisi `volatile` okuma sırasına dair gerçek tanımsız davranıştı.
- **Mesaj kodlayıcı:** `protocol.c` bilgisayarda gcc ile, her satır türü için en kötü durum
  değerleriyle test edildi (en uzun satır 60 bayt ≤ 63). Arayüz, firmware kodlayıcısının
  ürettiği satırlarla test edildi: parçalı okuma, yarım satır, bozuk satır ve sayaç sarması.
- **Analiz betiği:** `analyze.py` boş ve sentetik veriyle (yalnızca geçici klasörde, depoya
  konmadı) çalıştırıldı.
- **Kart üzerinde:**
  - İlk S0 denemesinde her basış iki olay üretti. Ardışık olaylar arasındaki 60–280 ms,
    bırakma sıçramasını gösterdi ve filtre düzeltildi.
  - Aynı denemede `t₄ − t₃ = 5576 µs` ölçüldü (teorik 5556 µs). Bu, t₄'ün UART TC'den geldiğini
    ve baud/saat ayarının doğru olduğunu gösterdi.
  - İlk S5 denemesinde `extra_load_us = 8373 µs` görüldü (hedef 5000). İş yükü bu ölçüme göre
    kalibre edildi.
- **Belgeleme:** Sıcaklık sensörü kalibrasyon adresleri CMSIS başlığında olmadığı için
  datasheet'ten (DS8626) alındı. Kartta sıcaklığın ve VDDA'nın makul olduğu kontrol edilmeli.

## Hangi öneriler değiştirildi ve neden?

- **Buton filtresi:** Slayttaki "yalnızca basış kenarı + son kabulden 30 ms" önerisi bırakma
  sıçramasını çözmüyordu (kartta gözlendi). 30 ms pencere, sayaçlar, ilk kenar kabulü ve kuyruk
  kurulum sırası korundu. İki kenar ve basılı/bırakılmış durumu eklendi.
- **Ölçüm kaydı:** Önerilen yöntem HAL/DMA tabanlıydı. Register seviyesi IT sürücüsüne
  uyarlandı. Havuzun tek sahibi UartTxTask yapıldı. Tampon kayda konmadı (4 KB tasarruf).
  Havuz dolunca eski kayıt ezilmiyor.
- **Canlı sonuç paketi kaldırıldı:** İlk sürüm her basıştan sonra sonucu (BTNR) canlı
  gönderiyordu. S5 verisi bunun ölçümü bozduğunu gösterdi (birikim basış başına +20 ms). Sonuçlar
  ölçüm penceresi sonunda dökülüyor.
- **Mesaj biçimi:** İlk sürüm ikili (binary) 64 bayttı. Şartnamedeki ASCII 63 + LF biçimine
  çevrildi.
- **Bilinçli sapmalar:** Şartnameden bilinçli sapmalar README'deki "Şartnameden sapmalar ve
  eklemeler" tablosunda gerekçeleriyle listelendi.

## Neler AI ile yapılmadı

- Ölçüm verisi üretilmedi. `measurements/` altındaki CSV'ler gerçek kart ölçümleridir (ya da
  henüz yalnızca başlık satırıdır); sentetik veri gerçek sonuç gibi sunulmadı.
