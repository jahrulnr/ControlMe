# 05 — Peta Pin & Perangkat Keras ESP32-CAM

> Peta pin kamera dan SD dicocokkan dengan **referensi Espressif LOKAL** yang sudah dibaca (bukan verifikasi online, bukan hasil uji unit). Unit persis di foto belum diverifikasi revisi/clone-nya.

## Modul (dari foto pengguna)

- **ESP32-CAM klasik gaya AI-Thinker** + **base programmer USB** (kemungkinan **ESP32-CAM-MB**) — revisi/clone persis tidak bisa dibuktikan dari foto.
- Diasumsikan nominal **4 MB flash + PSRAM onboard** (umumnya 4 MB; kapasitas persis wajib dibaca dari boot flash info & PSRAM init — jangan klaim ada/tidak sebelum dibaca).
- Kamera **OV2640 onboard** lewat kabel FFC + rail daya dari regulator papan — koneksi fixed pabrik, **tidak ada adapter DVP eksternal**.
- Slot **microSD onboard** — koneksi fixed pabrik ke peripheral SDMMC.
- Port USB pada base programmer adalah **UART bridge** (bukan USB native ESP32) — **GPIO1 (TX0) dan GPIO3 (RX0) direservasi** untuk programming/console.

## Pin Kamera Onboard (referensi koneksi fixed — bukan usulan rewire)

Pemetaan varian `CAMERA_MODEL_AI_THINKER` dari contoh resmi Espressif:

| Sinyal | GPIO |
|--------|------|
| PWDN | 32 |
| RESET | -1 (tidak ada GPIO khusus reset) |
| XCLK | 0 |
| SCCB_SDA | 26 |
| SCCB_SCL | 27 |
| D0 / Y2 | 5 |
| D1 / Y3 | 18 |
| D2 / Y4 | 19 |
| D3 / Y5 | 21 |
| D4 / Y6 | 36 |
| D5 / Y7 | 39 |
| D6 / Y8 | 34 |
| D7 / Y9 | 35 |
| VSYNC | 25 |
| HREF | 23 |
| PCLK | 22 |

- Ini adalah **koneksi onboard**, bukan pin header yang bisa dipakai ulang — jangan dianggap "pin bebas".
- XCLK: LEDC **timer0 channel0, 20 MHz** (dari contoh lokal; mode LEDC eksplisit — ESP32 klasik punya mode high-speed **dan** low-speed, jangan asumsikan LS-only).
- LED flash onboard = **GPIO4**; LED status = **GPIO33** (kandidat, tergantung varian board — bukan pin header bebas).

## Kartu SD Onboard (SDMMC slot 1, lebar 1-bit)

| Sinyal | GPIO | Catatan |
|--------|------|---------|
| CLK | 14 | fixed slot 1 |
| CMD | 15 | fixed slot 1 |
| DAT0 | 2 | satu-satunya jalur data di mode 1-bit |
| DAT1 | 4 | terhubung fisik; **tidak dipakai** untuk data di mode 1-bit |
| DAT2 | 12 | terhubung fisik; tidak dipakai untuk data; GPIO12 juga pin strapping |
| DAT3 | 13 | terhubung fisik; **wajib HIGH saat inisialisasi kartu** — bila LOW, kartu bisa masuk mode SPI |

- Mode **width=1 dikonfigurasi eksplisit** di driver; mode 4-bit akan menyita pin yang dipakai bersama servo/LED — bukan baseline.
- GPIO2 adalah pin **strapping** (harus LOW saat masuk download) — pull-up SD pada lini ini bisa mengganggu flashing.
- README SD_MMC lokal mensyaratkan **pull-up ~10 kΩ pada DAT3**; jangan menambah pull-up baru di GPIO12. **Pull-up yang terpasang di board wajib diinspeksi** — jangan asumsikan semua ada.
- ESP32 klasik tidak bisa memetakan ulang pin SDMMC bebas seperti seri lebih baru.

## Servo (conditional — berbagi pin dengan kartu SD)

| Peran | GPIO | Status |
|-------|------|--------|
| Servo PAN (utama) | 13 | **kandidat, conditional** — pin ini juga SD DAT3; lihat koordinasi di bawah |
| Servo TILT (opsional) | 12 | **conditional** — pin strapping VDD_SDIO; pull eksternal HIGH saat reset bisa memilih tegangan flash salah → boot gagal. Wajib lulus uji power-on/reset/brown-out dengan servo terpasang |

- **Tidak ada jaminan dua servo** untuk semua unit — baseline satu servo pan; tilt hanya setelah kualifikasi listrik. `servo_set` untuk channel yang tidak terkonfigurasi → `NOT_SUPPORTED`.
- GPIO13 bukan "pin bebas" — tetap terhubung ke DAT3 kartu.
- Saat PWM servo dimatikan untuk koordinasi SD, GPIO13 di-drive **HIGH**; tidak ada PWM valid → posisi servo mungkin tidak tertahan (bukan jaminan safety fisik).

## Koordinasi SD ↔ Servo (async, di DeviceActions — BUKAN blocking antar-owner)

Urutan pada **boot awal (sebelum PWM aktif)** dan **setiap re-init/remount SD**:

1. Suspend admission `servo_set` untuk channel 13.
2. Minta Servo owner mematikan PWM dan drive GPIO13 **HIGH**; tunggu ACK **tanpa memegang lock owner apa pun**.
3. Minta Storage owner mount SD slot1 width1.
4. Sukses → restore PWM ke desired terakhir + buka admission.
5. Gagal → `TIMEOUT`/`SD_UNAVAILABLE` eksplisit; GPIO13 **tetap HIGH** sampai pemulihan/reconfigure yang dikontrol pengguna — tidak resume PWM buta; `servo_set` → `RESOURCE_BUSY` selama handoff/pemulihan belum selesai.
6. Request servo13 yang masuk selama handoff → `RESOURCE_BUSY` (bukan menunggu selamanya).
7. Deadline bounded di tiap langkah; Storage owner tidak pernah menunggu Servo owner dan sebaliknya (tanpa deadlock).
8. Reboot → remount selesai **sebelum** PWM aktif. **Task mount awal wajib mendahului task PWM13** — termasuk saat SD tidak terpasang; mode tanpa-SD permanen adalah pilihan konfigurasi manual terpisah (pertanyaan review), bukan otomatis.

## Alternatif ekspander (OPSIONAL — trade-off review, bukan baseline)

PCA9685 (I2C PWM, 16 channel) kandidat untuk dua servo: **SDA=3, SCL=1** — hanya valid bila base programmer **dilepas atau diisolasi elektrik** dari kedua pin UART saat run, console serial dimatikan setelah takeover, aktivitas TX saat boot dikualifikasi, dan output PWM disabled selama init. Bukan plug-and-play, bukan GPIO gratis; tetap tabel opsi dengan batasannya.

## Pin yang Direservasi (jangan dipakai)

| GPIO | Alasan |
|------|--------|
| 1, 3 | UART0 TX/RX — programming/console via base USB (kecuali rute ekspander di atas dikualifikasi) |
| 6–11 | flash SPI internal |
| 16, 17 | PSRAM (bila terpasang) |
| 0 | XCLK kamera + strap download — jangan tambah aktuator |
| 2 | SD DAT0 + strap download |
| 4 | LED flash + SD DAT1 — **jangan PWM servo** |
| 12 | SD DAT2 + strap VDD_SDIO (kandidat tilt conditional) |
| 13 | SD DAT3 (kandidat pan conditional) |
| 14, 15 | SD CLK/CMD |
| 34–39 | input-only + jalur data kamera — bukan output bebas |

## Sumber Referensi (lokal, sudah dibaca) & Checklist Resmi (belum diambil)

**Sumber lokal yang diinspeksi (provenance: LOKAL, bukan online):**

- `~/.platformio/packages/framework-arduinoespressif32/package.json` — Arduino core 3.3.5.
- `~/.platformio/packages/framework-arduinoespressif32/libraries/ESP32/examples/Camera/CameraWebServer/camera_pins.h` (baris ~160–180) — pin AI_THINKER.
- `~/.platformio/packages/framework-arduinoespressif32/libraries/SD_MMC/README.md` — pemetaan SD + catatan pull-up DAT3/strap.
- `~/.platformio/packages/framework-arduinoespressif32-libs/esp32/include/soc/esp32/include/soc/sdmmc_pins.h` — pin fixed slot1.
- `~/.platformio/platforms/espressif32/boards/esp32cam.json` — MCU esp32, flash 4 MB, flag BOARD_HAS_PSRAM (**bukan** bukti kapasitas aktual).

**Referensi resmi terkait (dicatat, belum di-fetch/diverifikasi online):**

- https://github.com/espressif/arduino-esp32/tree/master/libraries/ESP32/examples/Camera/CameraWebServer
- https://github.com/espressif/arduino-esp32/tree/master/libraries/SD_MMC
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/sdmmc_host.html
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/sd_pullup_requirements.html
- https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf
- https://wiki.ai-thinker.com/esp32-cam

Item wajib cek sebelum pin final: strap VDD_SDIO (GPIO12), perilaku GPIO2 saat download, keberadaan pull-up DAT3 di unit, ukuran PSRAM aktual, mode LEDC HS/LS yang dipilih.
