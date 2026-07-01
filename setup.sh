#!/bin/bash

# ==============================================================================
# FaceAuth NextGen - Güvenli Kurulum ve Entegrasyon Betiği (v2.1)
# ==============================================================================

# Renk Tanımlamaları
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # Color Reset

echo -e "${BLUE}==================================================================${NC}"
echo -e "${BLUE}    FaceAuth NextGen - Güvenli Kurulum Betiği (v2.1 Elite Security)${NC}"
echo -e "${BLUE}==================================================================${NC}"

# 1. Root Yetkisi Kontrolü
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}[HATA] Bu betiğin root yetkisiyle (sudo ile) çalıştırılması gerekmektedir!${NC}"
    exit 1
fi

# 2. Kurulumu Gerçekleştiren Kullanıcının Tespiti
TARGET_USER=""
if [ -n "$SUDO_USER" ]; then
    TARGET_USER="$SUDO_USER"
else
    TARGET_USER=$(logname 2>/dev/null)
fi

if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = "root" ]; then
    echo -e "${YELLOW}[UYARI] Gerçek kullanıcı tespit edilemedi, varsayılan 'ads' kullanıcısı seçiliyor.${NC}"
    TARGET_USER="ads"
fi

echo -e "${GREEN}[OK] Kurulum yapılacak hedef kullanıcı: ${TARGET_USER}${NC}"

# 3. faceauth Kullanıcı Grubu Oluşturma
echo -e "${BLUE}[1/8] Kullanıcı grubu oluşturuluyor...${NC}"
if ! getent group faceauth >/dev/null; then
    groupadd faceauth
    echo -e "${GREEN}[OK] 'faceauth' grubu oluşturuldu.${NC}"
else
    echo -e "${YELLOW}[İPUCU] 'faceauth' grubu zaten mevcut.${NC}"
fi

# Kullanıcıyı gruba ekle
usermod -aG faceauth "$TARGET_USER"
echo -e "${GREEN}[OK] '${TARGET_USER}' kullanıcısı 'faceauth' grubuna eklendi.${NC}"

# 4. Sistem Dizinlerinin Oluşturulması
echo -e "${BLUE}[2/8] Güvenli sistem dizinleri (/var/lib/faceauth) oluşturuluyor...${NC}"
mkdir -p /var/lib/faceauth/models
mkdir -p /var/lib/faceauth/users

# 5. Modellerin Kopyalanması
echo -e "${BLUE}[3/8] Yapay Zeka modelleri kopyalanıyor...${NC}"
LOCAL_MODELS_DIR="./models"

if [ -d "$LOCAL_MODELS_DIR" ]; then
    for model in yunet.onnx sface.onnx minifas.onnx; do
        if [ -f "$LOCAL_MODELS_DIR/$model" ]; then
            cp "$LOCAL_MODELS_DIR/$model" /var/lib/faceauth/models/
            echo -e "  - $model kopyalandı."
        else
            echo -e "${YELLOW}[UYARI] Yerel model bulunamadı: $model${NC}"
        fi
    done
else
    echo -e "${RED}[HATA] Yerel 'models' dizini bulunamadı! Lütfen betiği projenin kök dizininde çalıştırın.${NC}"
    exit 1
fi

# 6. Projenin Derlenmesi
echo -e "${BLUE}[4/8] Proje derleniyor...${NC}"
# Cmake çalıştırma (Kullanıcı haklarıyla çalıştırmak en iyisidir, ancak geçici olarak root ile derliyoruz)
cmake -B build
cmake --build build

if [ $? -ne 0 ]; then
    echo -e "${RED}[HATA] Derleme işlemi başarısız oldu!${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] Derleme başarıyla tamamlandı.${NC}"

# 7. Dosyaların Sisteme Kurulması (Install)
echo -e "${BLUE}[5/8] İkilik dosyalar ve PAM modülü sisteme kuruluyor...${NC}"

# Daemon ve Canlı Test aracı
cp build/faceauth_daemon /usr/local/bin/
cp build/live_test /usr/local/bin/faceauth_live_test
chmod 755 /usr/local/bin/faceauth_daemon
chmod 755 /usr/local/bin/faceauth_live_test
echo -e "  - İkilik dosyalar kuruldu (/usr/local/bin/)."

# PAM Modülü (.so)
cp build/libpam_faceauth.so /usr/lib/security/pam_faceauth.so
chmod 644 /usr/lib/security/pam_faceauth.so
echo -e "  - PAM Modülü kuruldu (/usr/lib/security/pam_faceauth.so)."

# Yapılandırma Dosyası (.conf)
if [ -f /etc/faceauth.conf ]; then
    cp /etc/faceauth.conf /etc/faceauth.conf.bak
    echo -e "  - Mevcut yapılandırma dosyası yedeklendi (/etc/faceauth.conf.bak)."
fi
cp faceauth.conf /etc/faceauth.conf
chown root:faceauth /etc/faceauth.conf
chmod 644 /etc/faceauth.conf
echo -e "  - Yeni yapılandırma dosyası başarıyla kuruldu (/etc/faceauth.conf)."

# 8. Sahiplik ve İzinlerin Yapılandırılması (Güvenlik Sıkılaştırması)
echo -e "${BLUE}[6/8] Dizin sahiplikleri ve izinleri yapılandırılıyor (Güvenlik Sıkılaştırması)...${NC}"
chown -R root:faceauth /var/lib/faceauth

# Dizin izinleri (Sadece root ve faceauth grubu erişebilir)
chmod 770 /var/lib/faceauth
chmod 770 /var/lib/faceauth/users
chmod 775 /var/lib/faceauth/models
chmod 644 /var/lib/faceauth/models/*

# Eğer önceden kaydedilmiş yüz varsa sistem dizinine taşıyalım (Legacy Desteği)
if [ -f "./models/authorized_face.yml" ]; then
    cp "./models/authorized_face.yml" "/var/lib/faceauth/users/${TARGET_USER}.yml"
    chown root:faceauth "/var/lib/faceauth/users/${TARGET_USER}.yml"
    chmod 660 "/var/lib/faceauth/users/${TARGET_USER}.yml"
    echo -e "${GREEN}[OK] Mevcut yetkili yüz verisi sistem dizinine aktarıldı: ${TARGET_USER}.yml${NC}"
fi

echo -e "${GREEN}[OK] Güvenli izin yapılandırması tamamlandı.${NC}"

# 9. Systemd Servis Dosyasının Dağıtılması
echo -e "${BLUE}[7/8] Systemd servis dosyası kuruluyor...${NC}"

cat <<EOF > /etc/systemd/system/faceauth.service
[Unit]
Description=FaceAuth NextGen - C++ Arkaplan Yuz Tanima Servisi
After=multi-user.target

[Service]
Type=simple
User=${TARGET_USER}
RuntimeDirectory=faceauth
RuntimeDirectoryMode=0755
ExecStart=/usr/local/bin/faceauth_daemon
Restart=always
RestartSec=2
StandardOutput=journal
StandardError=journal
SyslogIdentifier=faceauth-daemon

[Install]
WantedBy=multi-user.target
EOF

chmod 644 /etc/systemd/system/faceauth.service
systemctl daemon-reload
echo -e "${GREEN}[OK] Systemd servisi başarıyla kuruldu.${NC}"

# 10. Servisin Başlatılması
echo -e "${BLUE}[8/8] Servis başlatılıyor ve aktif ediliyor...${NC}"
systemctl enable faceauth.service
systemctl restart faceauth.service

# Çalışma durumu kontrolü
sleep 1
if systemctl is-active --quiet faceauth.service; then
    echo -e "${GREEN}[OK] FaceAuth Daemon servisi arka planda başarıyla çalıştırıldı.${NC}"
else
    echo -e "${RED}[HATA] Servis başlatılamadı! Lütfen 'journalctl -u faceauth.service' komutunu kontrol edin.${NC}"
fi

echo -e "${BLUE}==================================================================${NC}"
echo -e "${GREEN}🎉 KURULUM BAŞARIYLA TAMAMLANDI!${NC}"
echo -e "${BLUE}==================================================================${NC}"
echo -e "${YELLOW}Şimdi Yapılması Gerekenler:${NC}"
echo -e " 1. Yüzünüzü sisteme kaydetmek için şu komutu çalıştırın (kullanıcı adınızla):"
echo -e "    ${GREEN}sudo faceauth_live_test ${TARGET_USER}${NC}"
echo -e "    (Kameraya bakarken 's' tuşuna basın, çıkış için 'q' tuşuna basın)"
echo -e ""
echo -e " 2. PAM entegrasyonunu aktif etmek için /etc/pam.d/sudo veya /etc/pam.d/hyprlock"
echo -e "    dosyasına şu satırı ekleyebilirsiniz (örnek):"
echo -e "    ${GREEN}auth sufficient pam_faceauth.so${NC}"
echo -e ""
echo -e " 3. Servis loglarını takip etmek için:"
echo -e "    ${GREEN}journalctl -u faceauth.service -f${NC}"
echo -e "${BLUE}==================================================================${NC}"
