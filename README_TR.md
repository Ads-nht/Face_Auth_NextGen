# 🛡️ AegisFace (FaceAuth NextGen)
### Linux İçin Yeni Nesil, Ultra Hızlı (<10ms) Yüz Tanıma PAM Modülü

[English](README.md) | [Türkçe](README_TR.md)

AegisFace; Linux sistemlerindeki (sudo, SDDM, Hyprlock vb.) eski Python tabanlı kimlik doğrulama araçlarının (Howdy gibi) yerini alacak, yüksek performanslı, güvenli ve hafif bir C++ / Saf C yüz tanıma sistemidir. **Daemon-Client (Servis-İstemci) mimarisi** ve OpenCV DNN ile optimize edilmiş ONNX modelleri kullanılarak tasarlanmış olup, gelişmiş çok katmanlı anti-spoofing (sahte yüz/bypass engelleme) teknolojisine sahiptir.

---

## 🚀 Öne Çıkan Özellikler

* **⚡ Çift Çalışma Modu (Performans ve RAM Tasarrufu):**
  * **Persistent Mod (Varsayılan / Hız Öncelikli):** Modeller sürekli RAM bellekte hazır tutulur. Kimlik doğrulamayı **<10ms** (ortalama **9.6 ms**) gibi ultra hızlı bir sürede tamamlar. RAM kullanımı: **~204 MB** (RSS).
  * **On-Demand Mod (İsteğe Bağlı / RAM Tasarrufu):** Modeller sadece kimlik doğrulama isteği geldiğinde diskten yüklenir ve işlem bitince RAM'den silinir. Boştayken RAM kullanımı **~20 MB**'a düşer. Kimlik doğrulama süresine disk hızınıza bağlı olarak **~150–300 ms** yük ekler.
* **🔄 Daemon-Client Mimarisi:** Modeller arka plan servisinde (`faceauth_daemon`) yönetilir ve istemciyle yerel bir UNIX soketi (`/run/faceauth/faceauth.sock`) üzerinden haberleşir. Bu sayede model yükleme gecikmesi yaşanmaz ve SDDM gibi ekran yöneticilerinin ev dizini kısıtlamaları (sandboxing) aşılmış olur.
* **🔒 Saf C PAM Modülü:** PAM istemci kütüphanesi (`pam_faceauth.so`) saf C (Pure C) diliyle yazılmıştır. Bu sayede kimlik doğrulama sırasında `libstdc++` çalışma zamanı bağımlılığı ortadan kaldırılarak, `dlclose()` çağrısı esnasında yaşanan ünlü `sudo` çökmeleri (Segmentation Fault / SIGSEGV) tamamen önlenmiştir.
* **🛡️ Gelişmiş Anti-Spoofing (Canlılık Analizi ve Saldırı Engelleme):**
  * **Yapay Zeka (MiniFASNet ONNX):** Kameraya gösterilen **kağıt baskı fotoğrafları (sabit resimler)** ve **akıllı telefon/tablet ekranlarından oynatılan videoları** anında tespit edip bloke eden pasif canlılık analizi.
  * **Doku ve Kenar Analizi (Texture & Edge Analysis):** Kağıt fotoğraflardaki baskı kaynaklı kenar bulanıklıklarını ve dijital ekranlardaki yüksek frekanslı Moire desenlerini (piksel kırınımlarını) Laplacian Varyansı ve HSV renk doygunluk limitleriyle yakalayan ultra hızlı yedek güvenlik motoru.
  * **Rijit Göz-Burun Hizalaması (Rigid Alignment):** Yüzdeki nirengi noktalarını burun ucu merkezli referans alarak konumlandırır; bu sayede 2D fotoğrafların kameraya eğilip bükülerek sarsılmasını matematiksel olarak bloke eder.
  * **Fizyolojik Titreşim Sınırları:** Normal insani mikro göz/kas hareketlerine izin veren, ancak sabit fotoğrafları (alt sınır: `1.5e-5`) ve kamerayı çılgınca sallayarak yapılan sarsıntı bypasslarını (üst sınır: `8.0e-4`) engelleyen akıllı varyans kontrolü.
  * **Soket Yetki Doğrulaması (Poka-Yoke):** UNIX soketine bağlanan istemcinin kullanıcı kimliğini (UID) Linux `SO_PEERCRED` ile doğrular. İstek atan kullanıcı `root` (UID 0), bir ekran yöneticisi sistemi veya hedef kullanıcının kendisi değilse yetkisiz istekleri anında reddeder. Yerel yetki yükseltme (privilege escalation) açıklarını önler.
* **⚙️ Adaptif Pozlama ve Kalibrasyon:** Kullanıcı yüzünü kaydederken (enrollment) ortam ışığına göre otomatik pozlama kalibrasyonu yapar ve gece ekran ışığında dahi kararlı çalışması için adaptif kontrast (CLAHE) dengelemesi uygular.

---

## 🛠️ Mimari Yapı

```mermaid
graph TD
    User([Kullanıcı / İşlem]) -->|Kimlik Doğrulamayı Tetikler| PAM[pam_faceauth.so (Saf C İstemci)]
    PAM -->|Unix Sokete Bağlanır| Sock[Unix Soket: /run/faceauth/faceauth.sock]
    Sock -->|Auth İsteği| Daemon[faceauth_daemon (C++ Arka Plan Servisi)]
    Daemon -->|Kameradan Kare Yakalar| Cam[V4L2 Kamera Aygıtı]
    Daemon -->|Yapay Zeka Çıkarımı| Models[ONNX Modelleri: YuNet, SFace, MiniFASNet]
    Models -->|Canlılık ve Yüz Analizi| Daemon
    Daemon -->|BAŞARILI / BAŞARISIZ Yanıtı| PAM
    PAM -->|Erişim İzni Verir/Reddeder| User
```

---

## 📋 Sistem Gereksinimleri

* **İşletim Sistemi:** Linux (Hyprland, KDE, GNOME vb. tüm masaüstü ortamlarıyla uyumlu)
* **Derleyici:** C++17 destekli GCC/G++, CMake (>= 3.10)
* **Gerekli Kütüphaneler:** 
  * OpenCV 4.x (DNN, VideoIO ve Imgproc modülleri aktif olmalı)
  * Linux PAM geliştirme kütüphaneleri (`libpam0g-dev` veya `pam`)

---

## ⚙️ Derleme ve Kurulum

### 1. Projeyi Derleyin:
```bash
cmake -B build
cmake --build build
```

### 2. Güvenli Kurulum Betiğini Çalıştırın:
`setup.sh` betiği derlemeden önce sistem gereksinimlerini otomatik olarak denetler (Poka-Yoke), sistem dizinlerini oluşturur, izinleri kısıtlar, modelleri taşır ve systemd servisini kaydeder:
```bash
sudo ./setup.sh
```

---

## 👤 Yüz Kaydı ve Kullanıcı Tanımlama

Yüzünüzü sisteme güvenli bir şekilde kaydetmek için görsel arayüzü çalıştırın:
```bash
sudo faceauth_live_test <kullanici_adiniz>
```
* Doğrudan kameraya bakın (sistem ışığı tarayacak ve otomatik kalibre olacaktır).
* Profilinizi kaydetmek için **`s`** tuşuna basın (Yüz şablonunuz secure olarak `/var/lib/faceauth/users/<kullanici>.yml` konumuna kaydedilir).
* Çıkmak için **`q`** tuşuna basın.

---

## 🔐 PAM Entegrasyonu

AegisFace'i sudo işlemlerinde veya ekran kilidinde aktif etmek için ilgili PAM yapılandırma dosyalarını düzenleyin.

### Sudo İçin:
`/etc/pam.d/sudo` dosyasını açın:
```bash
sudo nano /etc/pam.d/sudo
```
En üst satıra (ilk kural olarak) şu satırı ekleyin:
```ini
auth        sufficient  pam_faceauth.so
```
*Not: `sufficient` kuralı sayesinde yüz tanıma başarılı olursa şifre sormadan oturum açılır, başarısız olursa veya servis kapalıysa güvenli bir şekilde normal şifre ekranına düşer.*

---

## 🛠️ Yapılandırma Dosyası `/etc/faceauth.conf`

Daemon davranışını dinamik olarak ayarlayabilirsiniz. Parametreler her doğrulama isteğinde otomatik olarak yeniden okunur (servisi yeniden başlatmaya gerek yoktur):

```ini
[General]
# Anti-spoofing yöntemi: "minifas" (Yapay Zeka), "texture" (CV Laplacian/HSV) veya "none"
liveness_method = minifas
# Yüz eşleşmesi için benzerlik eşiği (0.0 - 1.0)
matching_threshold = 0.45
# RAM Tasarrufu Modu (true: boştayken ~20MB RAM, false: boştayken ~204MB RAM)
lazy_loading = false

[Texture]
# Fotoğraf/Ekran algılama limitleri
texture_min_laplacian = 380.0
texture_max_laplacian = 1500.0
texture_min_hsv_sat = 95.0
texture_max_hsv_sat = 190.0

[MiniFASNet]
# Yapay zeka canlılık eşiği (Real - Spoof logit farkı)
minifas_threshold = 1.5

[Environment]
# Doğrulama için gereken minimum ortalama parlaklık (0 - 255)
min_brightness = 35.0
# CLAHE adaptif pozlama dengelemesi
enable_adaptive_exposure = true
```

---

## 📄 Lisans

Bu proje MIT Lisansı altında lisanslanmıştır.

---

## 💖 Teşekkürler & Esinlenme

AegisFace, Linux için geliştirilen popüler Python tabanlı yüz tanıma sistemi [Howdy](https://github.com/boltgolt/howdy) projesinden büyük ölçüde esinlenmiştir.

AegisFace; Howdy'nin başlangıç gecikmelerini, Python bağımlılık karmaşıklığını ve dinamik bağlayıcı çökmelerini (`sudo` segfaults) çözmek amacıyla C++ ve C dilleriyle tamamen sıfırdan yazılmış olsa da, Howdy'nin Linux ekosistemine sorunsuz yüz tanıma getirme vizyonuna saygı duyar. Orijinal Howdy geliştiricilerine teşekkür ederiz!
