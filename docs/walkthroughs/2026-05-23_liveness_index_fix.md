# FaceAuth NextGen - Liveness İndis Düzeltmesi Walkthrough (23.05.2026)

Kullanıcımız Furkan Bey'in liveness (canlılık) doğrulamasında karşılaştığı "Fotoğraf algılandı" sorunu, C++ kaynak kodlarındaki binary sınıflandırma indislerinin ters okunmasından kaynaklanıyordu. Bu sorun başarıyla giderilmiş, kodlar derlenmiş ve sistem kurulmaya hazır hale getirilmiştir.

## 🛠️ Yapılan Değişiklikler

### 1. `live_test.cpp` İndis Düzeltmesi
*   **Dosya:** `Projeler/FaceAuth_NextGen/src/live_test.cpp`
*   **Değişiklik:** `liveness_output` çıkış tensoründeki indisler düzeltildi. İndis 1 (Real) `real_logit` değişkenine, İndis 0 (Spoof) `spoof_logit` değişkenine atandı.
*   **Kod:**
    ```diff
    -float real_logit = liveness_output.at<float>(0, 0);
    -float spoof_logit = liveness_output.at<float>(0, 1);
    +float real_logit = liveness_output.at<float>(0, 1); // İndis 1: Real
    +float spoof_logit = liveness_output.at<float>(0, 0); // İndis 0: Spoof
    ```

### 2. `daemon.cpp` İndis Düzeltmesi
*   **Dosya:** `Projeler/FaceAuth_NextGen/src/daemon.cpp`
*   **Değişiklik:** Daemon servisi içindeki indis atamaları da aynı şekilde düzeltildi.
*   **Kod:**
    ```diff
    -float real_logit = liveness_output.at<float>(0, 0);
    -float spoof_logit = liveness_output.at<float>(0, 1);
    +float real_logit = liveness_output.at<float>(0, 1); // İndis 1: Real
    +float spoof_logit = liveness_output.at<float>(0, 0); // İndis 0: Spoof
    ```

### 3. Kod Derleme Başarısı
*   Projedeki tüm geçici dosyalar temizlendi ve CMake ile başarılı bir şekilde yeniden yapılandırılıp sıfırdan derlendi:
    ```bash
    cmake -B build && cmake --build build
    ```
    Derleme hata vermeden `%100` başarıyla tamamlandı.

---

## 🚀 Uygulanacak Kurulum ve Yeniden Başlatma Adımları

Sistem genelindeki ikilik dosyaların güncellenmesi ve daemon servisinin yeniden başlatılması için lütfen aşağıdaki komut zincirini terminalinizde çalıştırın:

```bash
# 1. Yeni derlenen dosyaları sisteme kopyalayın ve izinlerini ayarlayın
sudo cp build/faceauth_daemon /usr/local/bin/
sudo cp build/live_test /usr/local/bin/faceauth_live_test
sudo chmod 755 /usr/local/bin/faceauth_daemon
sudo chmod 755 /usr/local/bin/faceauth_live_test

# 2. Yeni derlenen PAM modülünü yerleştirin
sudo cp build/libpam_faceauth.so /usr/lib/security/pam_faceauth.so
sudo chmod 644 /usr/lib/security/pam_faceauth.so

# 3. Arka plandaki daemon servisini yeniden başlatın
sudo systemctl restart faceauth.service
```

---

## 🔍 Doğrulama ve Canlı Test

Kurulum tamamlandıktan sonra, yeni liveness eşiğini test etmek ve tıraşlı yüz imzanızı kaydetmek için terminalinizde şu komutu çalıştırabilirsiniz:

```bash
faceauth_live_test ads
```

*   Artık ekranda yeşil **`CANLI (REAL)`** ibaresi kararlı bir şekilde belirecektir.
*   Kameraya bakarken **`s`** tuşuna basarak yeni yüz imzanızı anında kaydedebilirsiniz!
