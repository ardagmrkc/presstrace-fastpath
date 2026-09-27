# Kurulum

## 1. Donanım

| # | Bağlantı |
|---|---|
| 1 | STM32F407VG Discovery kartı, ST-LINK/V2 (kart üzerinde entegre) ile bilgisayara USB Mini-B ile bağlı. |
| 2 | Harici bir **USB-TTL (USB-UART) adaptör** (ör. FTDI FT232, CP2102, CH340) — kart **USB-UART köprüsü içermez**. |
| 3 | Adaptör **RX** ↔ kart **PA2** (USART2_TX) |
| 4 | Adaptör **TX** ↔ kart **PA3** (USART2_RX) |
| 5 | Adaptör **GND** ↔ kart **GND** |
| 6 | Adaptör 3.3V mantık seviyesinde olmalıdır (5V TTL adaptör kullanılıyorsa seviye kaydırıcı gerekir). |

Kullanıcı butonu (B1, mavi) ve gözlem LED'i (PD12, yeşil) kart üzerinde zaten mevcuttur, ek
kablolama gerekmez.

## 2. Araçlar

- IAR Embedded Workbench for ARM (EWARM) **9.70.4**. Proje bu sürümle oluşturuldu ve derlendi;
  başka bir sürümde açarsanız IAR projeyi dönüştürmeyi önerebilir.
- Git.
- Python 3.9+ ile `pandas` ve `matplotlib` (yalnızca `analysis/analyze.py` için):
  ```bash
  pip install pandas matplotlib
  ```
- Chrome veya Edge (Web Serial API için; Firefox/Safari desteklemez).
- İsteğe bağlı: UART hakemi testi için bir C derleyicisi (gcc).

## 3. Depoyu hazırlama

```bash
git clone https://github.com/ardagmrkc/presstrace-fastpath.git
```

FreeRTOS-Kernel kaynakları depoya dahildir (`firmware/Middlewares/FreeRTOS-Kernel`): resmi
`FreeRTOS/FreeRTOS-Kernel` deposunun `8be86d4` commit'i (V11.1.0+), yani ölçümlerin yapıldığı
sürüm. Ek bir adım gerekmez.

Karşılaştırma için önceki sürüm (115200, görev tabanlı yanıt yolu):
[presstrace-stm32](https://github.com/ardagmrkc/presstrace-stm32).

## 4. IAR projesini açma ve derleme

Proje hazır: `firmware/EWARM/odev_1.eww` (IAR EWARM 9.70.4 ile oluşturulan boş projeden
türetildi). Aşağıdakiler **önceden ayarlıdır**, elle yapmanız gerekmez:

| Ayar | Değer |
|---|---|
| Kaynak dosyalar | `Application` (Core/Src), `Startup`, `CMSIS`, `FreeRTOS` (+ `portable`) grupları |
| `Config` grubu | `FreeRTOSConfig.h`, `main.h`: yalnızca Workspace ağacında görünsünler diye eklendi; derlemeyi etkilemez |
| C/C++ Compiler → Defined symbols | `STM32F407xx`, `HSE_VALUE=8000000` (Release'te ayrıca `NDEBUG`) |
| C/C++ Compiler → Include directories | `Core/Inc`, `Drivers/CMSIS/Include`, `Drivers/CMSIS/Device/ST/STM32F4xx/Include`, `FreeRTOS-Kernel/include`, `FreeRTOS-Kernel/portable/IAR/ARM_CM4F` |
| Assembler → Include directories | `Core/Inc` — `portasm.s`, `FreeRTOSConfig.h`'yi include eder |
| Linker → Config | `stm32f407vg_flash.icf` (override) |
| Debugger → Driver | ST-LINK |

`STM32F407xx`, `stm32f4xx.h`'nin doğru cihaz başlığını seçmesi için zorunludur.
`HSE_VALUE=8000000` kart üzerindeki 8 MHz osilatörü yansıtır; tanımlanmazsa
`system_stm32f4xx.c` 25 MHz varsayar. Ayrıca `timers.c`, `event_groups.c`, `stream_buffer.c`,
`croutine.c` bilerek eklenmemiştir, çünkü bu projede kullanılmıyorlar (bkz. `FreeRTOSConfig.h`).

**Debug yapılandırması kullanıma hazırdır**, cihaz seçimi yapılmıştır:

| Yapılandırma | Cihaz | FPU | Optimizasyon | Durum |
|---|---|---|---|---|
| **Debug** | ST STM32F407VG | VFPv4 single precision | Low | Ölçümler ve kalibrasyon bununla yapıldı |
| Release | seçilmedi | — | High | Kullanılmadı; derlemek için önce cihaz seçilmeli (aşağıda) |

1. IAR'da **File → Open Workspace** ile `firmware/EWARM/odev_1.eww` dosyasını açın.
2. Workspace penceresinin üstündeki açılır listede **Debug** seçili olsun.
3. **Project → Make** (F7) ile derleyin. Beklenen sonuç: 0 hata, 0 uyarı.
4. **Project → Download and Debug** (Ctrl+D) ile karta yükleyin, ardından **Go** (F5) ile
   çalıştırın.

Release'i kullanmak isterseniz: Workspace'te projeye sağ tıklayın → **Options → General Options →
Target → Device** → **ST → STM32F4 → STM32F407 → ST STM32F407VG** ve FPU'nun **VFPv4 single
precision** olduğunu doğrulayın. FreeRTOS'un ARM_CM4F portu donanım FPU'su olmadan derlenmez
(`port.c`: `__ARMVFP__` kontrolü). Release farklı optimizasyonla derlediği için ek CPU işini
yeniden kalibre etmeniz gerekir (5. bölüm).

Komut satırından derleme (IAR kurulu klasörüne göre yolu uyarlayın):

```bash
"C:/iar/ewarm-9.70.4/common/bin/iarbuild.exe" firmware/EWARM/odev_1.ewp -build Debug -log warnings
```

**Senaryo seçimi:** Yeniden derleme gerekmez; senaryo arayüzün Senaryo panelinden seçilir
(bkz. 6. bölüm). [main.h](../firmware/Core/Inc/main.h) içindeki `ACTIVE_SCENARIO` yalnızca
kartın açılış senaryosudur. Bunu proje seçeneklerine **eklemeyin**; orada tanımlanırsa
`main.h`'deki değeri ezer.

**Optimizasyon seviyesi:** Debug yapılandırması *Low*, Release *High* optimizasyonla derler.
`workload_run()` süresi buna doğrudan bağlıdır. Bu yüzden kalibrasyonu ve tüm senaryo
ölçümlerini **aynı yapılandırmayla** yapın.

### Kartta doğrulananlar ve bilinen riskler

- **Kartta doğrulandı (27 Eylül 2026 ölçümleri, 230400):**
  - Saat ağacı ve UART baud hızı: 64 baytlık satırın ölçülen süresi `t₄ − t₃` = 2775–2781 µs,
    teorik değer 2773 µs (gerçek baud 230 769).
  - t₄'ün TC kesmesinden alındığı: TXE veya DMA "aktarım bitti" kesmesinden alınsaydı değer
    bir karakter (~43 µs) kısa çıkardı.
  - UART hakemi: her senaryoda `btn_direct + btn_latched = accepted`, `btn_fallback` = 0, sıra
    boşluğu ve hatalı satır 0 (baytlar karışmadı).
  - Telemetri periyotları: ortalama 99 999 / 19 999 / 9 999 µs; en uzun periyot nominalden en
    fazla +17 µs.
  - Senaryo komutu ve döküm; S0–S5'in tamamında kayıt bütünlüğü (bkz.
    [report.md](../analysis/report.md)).
- **Sıcaklık:** Kalibrasyon adresleri CMSIS başlığında olmadığı için DS8626'dan alındı. Değer
  arayüzün telemetri panelinde görünür, ancak bir termometreyle bağımsız olarak doğrulanmadı.
- **Release / cihazsız derleme:** Cihaz seçilmeden derlenirse tek hata kaynağı FreeRTOS'un FPU
  gerektiren port dosyalarıdır (`port.c`: `#error ... hardware floating point`, `portasm.s`:
  `s16`/`s31` tanımsız): 5 hata.
- **FreeRTOS sürümü:** Alt modül `8be86d4` commit'ine sabitlenmiştir. Alt modül bilerek
  güncellenirse `portable/IAR/ARM_CM4F/port.c` içindeki "Direct Routing" gereksinimleri
  değişebilir; sorun yaşarsanız
  [code-notes.md#direct-routing-freertos-vektör-kurulumu](code-notes.md#direct-routing-freertos-vektör-kurulumu)
  bölümüne bakın.

## 5. Kalibrasyon

`workload_run()` iterasyon sayıları ([workload.h](../firmware/Core/Inc/workload.h) içindeki
`WORKLOAD_ITERS_2MS` / `WORKLOAD_ITERS_5MS`) bu kartta ve Debug / Low derlemede **kalibre
edilmiştir**:

| Adım | Değer |
|---|---|
| İlk ölçüm (S5) | 140 000 iterasyon → 8373 µs |
| Oran | 0,0598 µs / iterasyon |
| `WORKLOAD_ITERS_2MS` (S4) | 33 440 |
| `WORKLOAD_ITERS_5MS` (S5) | 83 600 |

Kayıt `workload.h` içinde ve [report.md](../analysis/report.md) bölüm 2.5'tedir. Başka bir kartta,
derleyici sürümünde ya da yapılandırmada (ör. Release) çalışırken aşağıdaki adımlarla yeniden
doğrulayın:

1. Arayüzün Senaryo panelinden S4'ü seçin.
2. `extra_load_us` alanını arayüzden (TEL paketleri) izleyin — bu, `workload_run()`'ın
   gerçekte ne kadar sürdüğünü mikrosaniye cinsinden gösterir.
3. Hedef 2000 µs'den belirgin şekilde farklıysa, `WORKLOAD_ITERS_2MS` değerini orantılı olarak
   ayarlayın (`yeni_deger = eski_deger * 2000 / olculen_us`) ve tekrar derleyip doğrulayın.
4. S5 (`WORKLOAD_ITERS_5MS`, hedef 5000 µs) için aynı adımları tekrarlayın.
5. Kalibre edilmiş değerleri `workload.h` içine kaydedin ve raporun kalibrasyon bölümünü
   güncelleyin.

## 6. Web arayüzünü başlatma

1. Karta firmware yüklü ve USB-TTL adaptör takılıyken, `interface/index.html` dosyasını
   Chrome veya Edge ile açın (çift tıklama yeterlidir, sunucu gerekmez).
2. **Bağlan**'a tıklayın, tarayıcının seri port seçim penceresinde USB-TTL adaptörünüzü seçin.
3. Port ayarları otomatik olarak 230400 8N1'e sabitlenmiştir (bkz. [main.h](../firmware/Core/Inc/main.h)
   `UART_BAUDRATE`).
4. Arayüz bağlanır bağlanmaz kartın aktif senaryosunu sorgular. **Senaryo** panelinde
   vurgulanan düğme, kartın onayladığı senaryodur.
5. Senaryo değiştirmek için panelden bir düğmeye basın.
   - Listede CSV'ye kaydedilmemiş olay varsa önce sorulur.
   - Kart onaylayınca liste temizlenir. Değişiklikten önce kuyruğa girmiş eski senaryo
     paketleri yok sayılır.
   - Kart 1,5 s içinde yanıt vermezse kırmızı uyarı çıkar. Bu durumda USB-TTL **TX** → kart
     **PA3** bağlantısını kontrol edin.
6. Ölçüm sırasında TEL ve BTN satırları canlı gösterilir.
   - Her basış "Butona basıldı" bildirimini tetikler ve tabloya "ölçüm sürüyor" satırı ekler.
   - Telemetri panelinde gerçek periyot (ms/Hz), TX kuyruğu doluluğu ve `extra_load_us`
     görünür.
   - t₃, t₄ ve olayın durumu kartın kayıt havuzunda tutulur. Ölçümün kendi raporu hattı meşgul
     edip sonraki olayları geciktirmesin diye canlı gönderilmez.
7. Deney bitince **Ölçümü bitir ve kayıtları al**'a basın.
   - Kart S0'a geçer ve kayıtları (`REC`), pencere sayaçlarını (`CNT`) ve `END`'i gönderir.
   - Tablo, grafik ve **Gecikme dağılımı** paneli dolar. Tabloda bir satıra tıklayarak başka
     bir olayı seçebilirsiniz.
   - Durum panelindeki "Pencere sayaçları" dolar. Özet satırı olay durumlarını sayar
     (`ok / drop / tx_error / timeout`; `drop`'un nerede olduğu parantez içinde yazar: TX kuyruğu
     ya da buton kuyruğu).
   - Kayıt bütünlüğü bozuksa (kayıp satır, `accepted` ≠ kayıt sayısı, taşma) özet kırmızı olur.
8. **CSV Kaydet**'e basın. İki dosya iner; ikisini de `measurements/` altına taşıyın:
   - `S<n>.csv`: şartname biçimi, `scenario,event_id,t0_us,t1_us,t2_us,t3_us,t4_us,status`.
     Eksik zaman boş bırakılır. `status` şartnamedeki değerlerden biridir: `ok`, `drop`,
     `tx_error`, `timeout`.
   - `S<n>_counters.csv`: pencere sayaçları (`name,value`).

   Chrome ikinci indirme için bir kez izin isteyebilir.

## 7. Ölçüm prosedürü ve analiz

Her senaryo için (S0–S5):

1. Senaryo panelinden senaryoyu seçin ve kartın onayını bekleyin.
2. **5 s** ısınma bekleyin.
3. Butona en az **30 kez**, basışlar arasında en az **0,5 s** bırakarak basın. Rahat bir tempo
   saniyede bir basıştır.
4. **Ölçümü bitir ve kayıtları al**'a basın. Özet satırının "Kayıt bütünlüğü tam" dediğini kontrol
   edin. Pencere sayaçlarında hızlı yolun sağlığına bakın:
   - "BTN hızlı yol: hemen başladı" + "BTN hızlı yol: mandaldan başladı" = kabul edilen basış,
   - "BTN yedek yol (ButtonTask)" ve "BTN sonucu kuyruğa giremedi" = 0,
   - sıra boşluğu ve hatalı satır = 0.
5. **CSV Kaydet** ile iki dosyayı indirin ve `measurements/` altına taşıyın. Aynı senaryo yeniden
   ölçülürse eski dosyaların üstüne yazın. İndirme klasöründe aynı adlı dosya varsa tarayıcı yeni
   dosyayı `S5 (1).csv` gibi adlandırır; taşırken adı `S5.csv` olarak düzeltin.

Altı senaryo bitince analizi çalıştırın:

```bash
python analysis/analyze.py
```

Betik kendi konumuna göre `measurements/` klasörünü bulur, yani herhangi bir klasörden
çalıştırılabilir. Ürettikleri:

- `measurements/summary.csv`: senaryo başına olay ve durum sayıları, R min / ort / p95 / maks,
  deadline aşımı, aşama ortalamaları, pencere sayaçları, ölçülen telemetri hızı.
- `analysis/plots/r_vs_event.png`: olay numarası → R, 20 ms çizgisi.
- `analysis/plots/stages_by_scenario.png`: senaryo → aşama ortalamaları, yığılmış sütun.

`notes` sütunu 30'dan az olayı ve `accepted` ≠ kayıt sayısı durumunu işaretler. Sonuçların yorumu
[report.md](../analysis/report.md) içindedir.

## 8. UART hakemi testi (bilgisayarda)

Hızlı yolun ve UART hakeminin mantığı kart gerekmeden doğrulanabilir. Test, gerçek `uart.c` ve
`protocol.c` dosyalarını taklit USART register'ları ve taklit FreeRTOS ile derler (depo kökünden):

```bash
gcc -std=c11 -Wall -Wextra -I tests/host/mock -I firmware/Core/Inc tests/host/test_uart_arbiter.c firmware/Core/Src/uart.c firmware/Core/Src/protocol.c -o test_uart_arbiter
```

Çalıştırınca 8 senaryonun her biri için sonuç yazar ve son satırda `TUM TESTLER GECTI (0 hata)`
beklenir. Senaryoların listesi [README.md](../README.md#5-doğrulama) içindedir.
