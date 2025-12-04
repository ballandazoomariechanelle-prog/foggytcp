/* Copyright (C) 2024 Hong Kong University of Science and Technology

This repository is used for the Computer Networks (ELEC 3120)
course taught at Hong Kong University of Science and Technology.

No part of the project may be copied and/or distributed without
the express permission of the course staff. Everyone is prohibited
from releasing their forks in any public places. */

/*
 * This file implements the foggy-TCP backend. The backend runs in a different
 * thread and handles all the socket operations separately from the application.
 *
 * This is where most of your code should go. Feel free to modify any function
 * in this file.
 */

#include <assert.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h> // Nécessaire pour clock_gettime

#include "foggy_backend.h"
#include "foggy_function.h"
#include "foggy_packet.h"
#include "foggy_tcp.h"

#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))

 /**
  * Fonction utilitaire pour obtenir le temps actuel en millisecondes.
  */
long get_time_in_ms() {
    struct timespec ts;
    // Utiliser CLOCK_MONOTONIC pour un temps qui avance toujours, même si l'horloge système change
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


/**
 * Tells if a given sequence number has been acknowledged by the socket.
 *
 * @param sock The socket to check for acknowledgements.
 * @param seq Sequence number to check.
 *
 * @return 1 if the sequence number has been acknowledged, 0 otherwise.
 */
int has_been_acked(foggy_socket_t* sock, uint32_t seq) {
    int result;
    while (pthread_mutex_lock(&(sock->window.ack_lock)) != 0) {
    }
    // Utilise la macro 'after' de foggy_function.h
    result = after(sock->window.last_ack_received, seq);
    pthread_mutex_unlock(&(sock->window.ack_lock));
    return result;
}

/**
 * Checks if the socket received any data.
 *
 * It first peeks at the header to figure out the length of the packet and
 * then reads the entire packet.
 *
 * @param sock The socket used for receiving data on the connection.
 * @param flags Flags that determine how the socket should wait for data.
 * Check `foggy_read_mode_t` for more information.
 */
void check_for_pkt(foggy_socket_t* sock, foggy_read_mode_t flags) {
    foggy_tcp_header_t hdr;
    uint8_t* pkt;
    socklen_t conn_len = sizeof(sock->conn);
    ssize_t len = 0;
    uint32_t plen = 0, buf_size = 0, n = 0;

    while (pthread_mutex_lock(&(sock->recv_lock)) != 0) {
    }
    switch (flags) {
    case NO_FLAG:
        len = recvfrom(sock->socket, &hdr, sizeof(foggy_tcp_header_t), MSG_PEEK,
            (struct sockaddr*)&(sock->conn), &conn_len);
        break;

        // Fallthrough.
    case NO_WAIT:
        len = recvfrom(sock->socket, &hdr, sizeof(foggy_tcp_header_t),
            MSG_DONTWAIT | MSG_PEEK, (struct sockaddr*)&(sock->conn),
            &conn_len);
        break;

    default:
        perror("ERROR unknown flag");
    }
    if (len >= (ssize_t)sizeof(foggy_tcp_header_t)) {
        plen = get_plen(&hdr);
        pkt = (uint8_t*)malloc(plen);
        while (buf_size < plen) {
            n = recvfrom(sock->socket, pkt + buf_size, plen - buf_size, 0,
                (struct sockaddr*)&(sock->conn), &conn_len);
            buf_size = buf_size + n;
        }
        on_recv_pkt(sock, pkt);
        free(pkt);
    }
    pthread_mutex_unlock(&(sock->recv_lock));
}

void* begin_backend(void* in) {
    foggy_socket_t* sock = (foggy_socket_t*)in;
    int death, buf_len, send_signal;
    uint8_t* data;

    long current_time_ms, last_send_time_ms, elapsed_time_ms;

    while (1) {
        while (pthread_mutex_lock(&(sock->death_lock)) != 0) {
        }
        death = sock->dying;
        pthread_mutex_unlock(&(sock->death_lock));

        // ------------------------------------------------------------------
        // NOUVELLE LOGIQUE: VÉRIFICATION ET GESTION DU TIMER DE RETRANSMISSION
        // ------------------------------------------------------------------
        if (sock->window.retransmit_timeout > 0 && !sock->send_window.empty()) {

            current_time_ms = get_time_in_ms();

            // Convertir le temps de départ struct timespec en ms
            last_send_time_ms = sock->window.last_send_time.tv_sec * 1000 +
                sock->window.last_send_time.tv_nsec / 1000000;

            elapsed_time_ms = current_time_ms - last_send_time_ms;

            // Si le temps écoulé est supérieur ou égal au RTO
            if (elapsed_time_ms >= sock->window.retransmit_timeout) {
                // Timeout détecté! Le timer doit être géré sous lock si nécessaire, 
                // mais l'appel à on_retransmit_timer() le gère déjà.
                on_retransmit_timer(sock);
            }
        }
        // ------------------------------------------------------------------


        while (pthread_mutex_lock(&(sock->send_lock)) != 0) {
        }
        buf_len = sock->sending_len;

        if (!sock->send_window.empty()) {
            // printf("Sending window is not empty\n");
            send_pkts(sock, NULL, 0); // Tente d'envoyer les paquets en attente
            check_for_pkt(sock, NO_WAIT);
        }

        if (death && buf_len == 0 && sock->send_window.empty()) {
            break;
        }

        if (buf_len > 0) {

            data = (uint8_t*)malloc(buf_len);
            memcpy(data, sock->sending_buf, buf_len);
            sock->sending_len = 0;
            free(sock->sending_buf);
            sock->sending_buf = NULL;
            pthread_mutex_unlock(&(sock->send_lock));
            send_pkts(sock, data, buf_len);
            free(data);
        }
        else {
            pthread_mutex_unlock(&(sock->send_lock));
        }

        check_for_pkt(sock, NO_WAIT);

        while (pthread_mutex_lock(&(sock->recv_lock)) != 0) {
        }

        send_signal = sock->received_len > 0;

        pthread_mutex_unlock(&(sock->recv_lock));

        if (send_signal) {
            pthread_cond_signal(&(sock->wait_cond));
        }

        usleep(1000); // Délai de 1ms pour réduire la charge CPU et laisser le temps au timer d'avancer
    }

    pthread_exit(NULL);
    return NULL;
}


// --- Implémentation des Fonctions de Timer Go-Back-N ---

/**
 * Démarre le timer de retransmission pour le paquet SendBase.
 */
void start_retransmit_timer(foggy_socket_t* sock) {
    // Si la fenêtre est vide, pas besoin de timer.
    if (sock->send_window.empty()) {
        stop_retransmit_timer(sock);
        return;
    }

    // Le RTO_INITIAL est déjà défini dans foggy_function.h
    sock->window.retransmit_timeout = RTO_INITIAL;

    // Enregistre le temps actuel.
    struct timespec current_time;
    // On met à jour l'heure de départ, car c'est une NOUVELLE tentative d'envoi de SendBase
    if (clock_gettime(CLOCK_MONOTONIC, &current_time) == 0) {
        sock->window.last_send_time = current_time;
    }
}

/**
 * Arrête le timer de retransmission.
 */
void stop_retransmit_timer(foggy_socket_t* sock) {
    // Réinitialise le délai à 0 pour indiquer que le timer est inactif.
    sock->window.retransmit_timeout = 0;
}