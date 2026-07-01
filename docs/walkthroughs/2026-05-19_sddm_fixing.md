# SDDM Entegrasyonu ve İzin Sorunu Çözümü (19.05.2026)

## 📌 Problem Analizi
Önceki aşamalarda `pam_faceauth` modülü `sudo` ve `hyprlock` altında çalışırken, SDDM giriş ekranında (Greeter) tetiklenmiyordu veya çalışmıyordu.
Loglar ve dizin izinleri analiz edildiğinde şu kritik bulgulara ulaşıldı:
1. **/home/ads İzinleri:** `/home/ads` dizini `700` (`drwx------`) yetkilerine sahipti. 
2. **SDDM Sınırlamaları (Sandboxing):** SDDM (sddm-helper) sistemi güvenlik nedeniyle kullanıcının ev dizinine (`/home/ads`) erişemez (AppArmor/SELinux veya Systemd `ProtectHome=yes` ayarları nedeniyle). Eski soket yolu `/home/ads/.../faceauth.sock` üzerinde olduğu için SDDM'in bu sokete bağlanması tamamen engelleniyordu.
3. **Eski systemd Log Yapısı:** `faceauth.service` içerisindeki `StandardOutput=syslog` tanımı sistem tarafından demode kabul ediliyor ve uyarı veriyordu.

## 🛠️ Uygulanan Çözüm Adımları
Bu engeli aşmak için **Daemon-Client** iletişim kanalını ev dizininden bağımsız, sistem genelinde güvenli ve geçici bir alana taşıdık:

1. **Soket Yolunun Güncellenmesi:** 
   Soket yolu `/home/ads/Antigravity/Projeler/FaceAuth_NextGen/faceauth.sock` adresinden, standart Linux çalışma zamanı dizini olan **`/run/faceauth/faceauth.sock`** adresine çekildi.
   * `src/pam_faceauth.cpp` güncellendi.
   * `src/daemon.cpp` güncellendi.

2. **Systemd Servis İyileştirmesi (`RuntimeDirectory`):**
   Daemon servisi `ads` kullanıcısı ile çalıştığı için normal şartlarda doğrudan `/run/` altına yazamazdı. Bunu aşmak için `faceauth.service` birimine Systemd'nin en güvenli ve temiz yetkilendirme mekanizmalarından biri entegre edildi:
   ```ini
   RuntimeDirectory=faceauth
   RuntimeDirectoryMode=0755
   ```
   * Bu sayede Systemd servis başlarken otomatik olarak `/run/faceauth` dizinini oluşturur, sahipliğini `ads` kullanıcısına verir ve servis kapandığında bu geçici dizini güvenli bir şekilde siler.
   * Demode `syslog` log tanımları `journal` ile güncellendi.

3. **Yeniden Derleme:**
   `cd build && cmake .. && make` komutuyla hem daemon (`faceauth_daemon`) hem de PAM modülü (`libpam_faceauth.so`) başarıyla sıfırdan derlendi.

## 🚀 Devreye Alma ve SDDM Yapılandırma Adımları (Sudo Yetkileri)

Sistem seviyesindeki dosyaların güncellenmesi ve servislerin yeniden başlatılması için aşağıdaki komutların çalıştırılması gerekmektedir:

### 1. Yeni PAM Modülünün Kurulması
Derlenen yeni modülü sistem kütüphanesine kopyalayıp gerekli izinleri tanımlayalım:
```bash
sudo cp build/libpam_faceauth.so /usr/lib/security/pam_faceauth.so
sudo chmod 755 /usr/lib/security/pam_faceauth.so
```

### 2. Systemd Servis Dosyasının Güncellenmesi ve Servisin Başlatılması
Yeni `faceauth.service` dosyasını `/etc/systemd/system/` altına taşıyalım ve daemon'ı yeni soket yoluyla tetikleyelim:
```bash
sudo cp faceauth.service /etc/systemd/system/faceauth.service
sudo systemctl daemon-reload
sudo systemctl restart faceauth.service
```

### 3. Servisin Çalıştığının Teyit Edilmesi
Soketin `/run/faceauth/` altında başarıyla oluşturulduğunu ve izinlerinin doğru olduğunu kontrol edelim:
```bash
ls -la /run/faceauth/
```
*(Çıktıda `faceauth.sock` dosyasının sahibi `ads` ve izinleri `srw-rw-rw-` olmalıdır.)*

### 4. SDDM PAM Yapılandırmasının Yapılması
`/etc/pam.d/sddm` dosyasına `pam_faceauth.so` kuralını eklememiz gerekiyor.
Dosyayı açın:
```bash
sudo nano /etc/pam.d/sddm
```
Ve dosyanın en üstüne (ikinci satır, `#%PAM-1.0` ifadesinin hemen altına) şu satırı ekleyin:
```ini
auth        sufficient  pam_faceauth.so
```
*Bu kural sayesinde yüz tanıma başarılı olduğunda doğrudan oturum açılacak (`sufficient`), başarısızlık durumunda ise standart şifre ekranına geri düşülecektir.*
