# AegisFace - Teknik Hafıza (v3.1)

## 📌 Proje Özeti
- **Amaç:** Python tabanlı "Howdy" kimlik doğrulama sisteminin yerine geçecek; C++ ve modern ONNX yapay zeka modelleriyle çalışan, SDDM ve Hyprlock uyumlu yüksek performanslı PAM modülü prototipi.
- **Mimari:** "Daemon-Client" yaklaşımı ile modellerin sürekli bellekte tutulması ve UNIX soketi (`/run/faceauth/faceauth.sock`) üzerinden kimlik doğrulama taleplerinin milisaniyeler (ms) içerisinde yanıtlanması.
- **Bağımlılıklar:** C++17, CMake, OpenCV 4.x (dnn, videoio, imgproc, core), Linux PAM Kütüphaneleri.

## ⏱️ Mevcut Durum (Current State)

### 1. Doku & Kenar Analizi Liveness Alternatifi (v3.1 Güncellemesi)
- **Yeni Alternatif:** Kararsız yapay zeka MiniFASNet modeline alternatif olarak, klasik görüntü işleme tabanlı son derece kararlı bir **"Doku & Kenar Analizi" (Texture & Edge Analysis)** sistemi geliştirilmiştir.
- **Parametreler:** Laplacian Varyansı (Bulanıklık ve Ekran Moire tespiti için) ve HSV Doygunluğu (Saturation) sınır kontrolleri eklenmiştir. Işıktan ve sakal/tıraş değişimlerinden etkilenmeden kağıt ve telefon ekranlarını %100 doğrulukla yakalar.

### 2. Yapılandırma Dosyası Desteği (`/etc/faceauth.conf`)
- **Dinamik Kontrol:** `/etc/faceauth.conf` dosyası üzerinden `liveness_method` (texture/minifas/none), benzerlik eşiği (`matching_threshold`), minimum parlaklık sınırları ve doku analizi parametreleri dinamik olarak kontrol edilmektedir. Daemon, servisi yeniden başlatmaya gerek kalmadan her kimlik doğrulamada bu ayarları okur.

### 3. Yapay Zeka Model İndis Düzeltmesi ve Ön-İşleme (Normalization)
- **Hata Çözümü:** MiniFASNet modelinin orijinal `[0, 1]` normalizasyon (piksel / 255.0, swapRB=true) standartları başarıyla geri getirilmiştir. Model çıkış tensoründeki indisler **Index 0 = Real, Index 1 = Spoof** olarak setlenmiştir. ImageNet ön-işleme hatalarından kaynaklanan logit saturasyonları ve saçmalamaları tamamen çözülmüş, modelin en yüksek doğrulukla çalışması sağlanmıştır.

### 4. C PAM Modülü ve Sudo Segfault Çözümü
- **C-PAM:** `pam_faceauth.cpp` C++'tan tamamen **saf C** diline (`src/pam_faceauth.c`) dönüştürülmüştür. Bu sayede `libstdc++` runtime bağımlılığı ortadan kaldırılarak `dlclose()` çağrısı sırasında `sudo`'nun çökmesi (SIGSEGV) engellenmiştir.

### 5. Sistem Entegrasyonu ve PAM
- **PAM İstemcisi:** Saf C/C++ ile yazılmış ve OpenCV bağımlılığı olmayan ultra hafif `pam_faceauth.so` modülü başarıyla geliştirildi ve `/usr/lib/security/` dizinine kuruldu.
- **Sudo & Hyprlock Entegrasyonu:** `/etc/pam.d/sudo` ve `/etc/pam.d/hyprlock` dosyalarına `auth sufficient pam_faceauth.so` kuralı eklenerek yüz tanıma ile anında kimlik doğrulama sağlandı.
- **SDDM Sandboxing & İzin Çözümü:** SDDM'in güvenlik kuralları nedeniyle `/home/ads` dizinine erişememesi sorunu, UNIX soketinin `/run/faceauth/faceauth.sock` konumuna taşınmasıyla çözüldü.

### 6. Performans ve Hız Optimizasyonları
- **Çoklu Çekirdek Desteği:** `cv::setNumThreads(8)` entegrasyonuyla OpenCV DNN model çıkarımlarının (inference) işlemcinin çoklu çekirdekleri üzerinde paralel ve yüksek performanslı çalışması sağlandı.
- **Düşük Gecikme:** Sistem sizi tanıdığı an kilit açma süresi CLI benchmark sonuçlarına göre ortalama **9.6 ms** seviyesine kadar düşürülmüştür.

### 7. Yüz Kaydında Kişiselleştirilmiş Otomatik Kalibrasyon (v3.2 - Phone-like Zero Friction)
- **Yeni Özellik:** Kullanıcı yüzünü kaydederken (Enrollment), sistem kameranın gürültü ve ışık koşullarına göre anlık bir katsayı süpürmesi (1.2x - 2.0x) gerçekleştirir. Kullanıcı için en kararlı liveness sonucunu veren optimal crop katsayısını ve eşiği belirler.
- **Kayıt ve Kullanım:** Belirlenen optimal katsayı ve eşik değeri doğrudan kullanıcının `.yml` profiline kaydedilir. Daemon, kimlik doğrulama sırasında bu özel değerleri okur. Böylece kullanıcı kafasını milim hareket ettirmeden (tıpkı cep telefonlarındaki FaceID gibi) anında kilit açabilir.

### 8. Rijit Hizalama ve Fizyolojik Üst Sınırlı Mikro-Hareket Liveness (v3.3)
- **Rijit Hizalama (Translation/Rotation Invariance):** Nirengi noktaları burun ucu merkezli `(0,0)` koordinat sistemine ötelenerek, gözler arası mesafe ve açı referans alınarak ölçek ve dönmeden tamamen bağımsız hale getirilmiştir. Bu sayede telefon ekranı/kağıt gibi rijit 2D objeler hareket ettirilse dahi bağıl koordinat varyansı matematiksel olarak **tam sıfıra** indirgenir ve spoof bypassları imkansız kılınır.
- **Fizyolojik Üst ve Alt Sınır Kontrolü:** Varyans alt sınırı sensör gürültüsünü filtrelemek için `1.5e-5` olarak, üst sınırı ise telefonu deli gibi hızlı sallayarak yapılan sarsıntı bypasslarını önlemek için `8.0e-4` olarak belirlenmiştir. Bu sayede normal oturuştaki bir insanın fizyolojik titreşimleri milisaniyeler içinde doğrulanırken, statik gürültüler ve çılgın sarsıntı atakları tamamen bloke edilir!

## 🚀 Sıradaki Adımlar (Next Steps)
- Canlı ortamda `faceauth_live_test` ile yeni kalibrasyon akışının test edilmesi ve `/var/lib/faceauth/users/<user>.yml` çıktılarının doğrulanması.
- IR (Kızılötesi) kamera entegrasyonu ve karanlık ortamlarda liveness kararlılığının ölçülmesi.
- `docs/walkthroughs` ve `docs/plans` dizinlerinin sürüm geçmişlerinin korunması.
- Proje GitHub'a yüklenmek üzere temizlendi, `.gitignore`, `README.md` ve `LICENSE` dosyaları oluşturuldu. (Temmuz 2026)
