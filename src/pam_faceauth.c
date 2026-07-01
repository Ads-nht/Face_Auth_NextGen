#include <security/pam_modules.h>
#include <security/pam_ext.h>
#include <security/pam_appl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SOCKET_PATH "/run/faceauth/faceauth.sock"

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    // Check for arguments
    int wait_for_enter = 0;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "wait_for_enter") == 0) {
            wait_for_enter = 1;
        }
    }

    // 1. Get target username from PAM context
    const char *username = NULL;
    if (pam_get_user(pamh, &username, NULL) != PAM_SUCCESS || !username) {
        pam_syslog(pamh, LOG_ERR, "Kullanici adi alinamadi!");
        return PAM_AUTH_ERR;
    }

    // 2. Prompt message
    pam_info(pamh, "[FaceAuth] Yuz tarayici baslatiliyor, lütfen kameraya bakin...");

    // 3. Create socket
    int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd == -1) {
        pam_syslog(pamh, LOG_ERR, "Soket olusturulamadi!");
        return PAM_AUTH_ERR;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    // 4. Connect to the Daemon socket
    if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        // If Daemon is sleeping or not running, fallback gracefully immediately
        pam_info(pamh, "[FaceAuth] Servis aktif degil. Standart sifre girisine yonlendiriliyorsunuz...");
        close(client_fd);
        return PAM_AUTH_ERR;
    }

    // 5. Send AUTH_REQUEST with username
    char request[256];
    snprintf(request, sizeof(request), "AUTH_REQUEST:%s", username);
    if (send(client_fd, request, strlen(request), 0) == -1) {
        pam_syslog(pamh, LOG_ERR, "Istek gonderilemedi!");
        close(client_fd);
        return PAM_AUTH_ERR;
    }

    // 6. Read response
    char buffer[128];
    memset(buffer, 0, sizeof(buffer));
    ssize_t bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    close(client_fd);

    if (bytes_received > 0) {
        // Strip trailing spaces/newlines
        int len = strlen(buffer);
        while (len > 0 && (buffer[len - 1] == ' ' || buffer[len - 1] == '\n' || buffer[len - 1] == '\r' || buffer[len - 1] == '\t')) {
            buffer[len - 1] = '\0';
            len--;
        }

        if (strcmp(buffer, "SUCCESS") == 0) {
            if (wait_for_enter) {
                char *resp = NULL;
                // Wait for the user to press Enter before completing authentication
                int rc = pam_prompt(pamh, PAM_PROMPT_ECHO_ON, &resp, "[FaceAuth] Yuz onaylandi. Giris yapmak icin ENTER'a basin: ");
                if (resp) {
                    free(resp); // Free response allocated by pam_prompt to prevent memory leak
                }
                if (rc != PAM_SUCCESS) {
                    return PAM_AUTH_ERR;
                }
            }
            pam_info(pamh, "[FaceAuth] Kimlik dogrulama basarili! Kilit acildi.");
            return PAM_SUCCESS;
        } else {
            pam_info(pamh, "[FaceAuth] Kimlik dogrulanamadi veya fotograf algilandi.");
            return PAM_AUTH_ERR;
        }
    }

    return PAM_AUTH_ERR;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    return PAM_SUCCESS;
}
