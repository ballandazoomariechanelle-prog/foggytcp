/* Copyright (C) 2024 Hong Kong University of Science and Technology */

#include <deque>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include "foggy_function.h"
#include "foggy_backend.h"


#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))

#define DEBUG_PRINT 1
#define debug_printf(fmt, ...) \
    do {\
        if (DEBUG_PRINT) fprintf(stdout, fmt, ##__VA_ARGS__);\
    } while (0)
// ----------------------------------------------------------------------
// -------------------- FONCTIONS D'ASSISTANCE --------------------------
// ----------------------------------------------------------------------

/**
 * Retransmet le paquet SendBase. Utilisé pour Fast Retransmit (3 ACKs Dupliqués).
 * Cette fonction doit être déclarée dans foggy_function.h.
 */
void retransmit_send_base(foggy_socket_t* sock) {
    if (sock->send_window.empty()) return;

    send_window_slot_t& lost_slot = sock->send_window.front();
    foggy_tcp_header_t* hdr = (foggy_tcp_header_t*)lost_slot.msg;

    debug_printf("Fast Retransmit: Sending packet %d\n", get_seq(hdr));

    // Renvoyer le paquet SendBase
    sendto(sock->socket, lost_slot.msg, get_plen(hdr), 0,
        (struct sockaddr*)&(sock->conn), sizeof(sock->conn));

    // Marquer l'envoi pour éviter une double retransmission non désirée
    lost_slot.is_sent = 1;
}

// Fonction de retransmission appelée par le timer (CP2 - GBN)
void on_retransmit_timer(foggy_socket_t* sock) {
    if (sock->send_window.empty()) return;

    // --- GESTION DU TIMEOUT RTO (Non requise pour la perte CP3, mais change l'état RENO) ---
    // Selon l'énoncé du CP3, on ne détecte pas la perte par RTO. 
    // Cependant, si le RTO se déclenche, il doit réinitialiser l'état de congestion.

    // 1. Appliquer les règles de TCP Reno en cas de Timeout RTO (même si l'événement n'est pas utilisé pour déclencher la perte)
    uint32_t current_window = MIN(sock->window.congestion_window, sock->window.advertised_window);
    sock->window.ssthresh = MAX(current_window / 2, 2 * MSS);
    sock->window.congestion_window = MSS;
    sock->window.reno_state = RENO_SLOW_START;
    sock->window.dup_ack_count = 0;

    debug_printf("Timeout detected. Transition to SLOW START. ssthresh=%d, cwnd=%d\n", sock->window.ssthresh, sock->window.congestion_window);

    // 2. Re-démarrer le timer immédiatement
    start_retransmit_timer(sock);

    // 3. Préparation à la retransmission : marquer tous les paquets dans la fenêtre comme non envoyés
    debug_printf("Retransmitting all packets from SendBase %d\n", sock->window.send_base);

    std::deque<send_window_slot_t>::iterator it = sock->send_window.begin();
    for (; it != sock->send_window.end(); ++it) {
        it->is_sent = 0;
    }

    // 4. Envoyer la fenêtre
    transmit_send_window(sock);
}


// ----------------------------------------------------------------------
// -------------------- LOGIQUE TCP - FENÊTRE GLISSANTE -----------------
// ----------------------------------------------------------------------

/**
 * Met à jour les informations du socket pour un paquet reçu.
 * @param sock Le socket.
 * @param pkt Le paquet de données reçu.
 */
void on_recv_pkt(foggy_socket_t* sock, uint8_t* pkt) {
    debug_printf("Received packet\n");
    foggy_tcp_header_t* hdr = (foggy_tcp_header_t*)pkt;
    uint8_t flags = get_flags(hdr);
    uint32_t old_send_base = sock->window.send_base; // Sauvegarde de SendBase pour la comparaison

    // --- Gestion ACK (Côté Émetteur) ---
    if (flags & ACK_FLAG_MASK) {
        uint32_t ack = get_ack(hdr);
        printf("Receive ACK %d\n", ack);

        // [FLOW CONTROL] Mise à jour du RWND (Advertised Window)
        sock->window.advertised_window = get_advertised_window(hdr);

        // 1. Vérifier si l'ACK est nouveau et fait avancer la fenêtre.
        if (after(ack, old_send_base)) { // NOUVEL ACK

            // --- LOGIQUE GBN (Avancer la fenêtre) ---
            sock->window.send_base = ack;
            receive_send_window(sock);

            // 4. Redémarrer le timer s'il reste des paquets non acquittés
            if (!sock->send_window.empty()) {
                start_retransmit_timer(sock);
            }
            else {
                stop_retransmit_timer(sock);
            }

            // --- LOGIQUE TCP RENO (Croissance de CWND) ---
            // Réinitialiser le compteur après un nouvel ACK
            sock->window.dup_ack_count = 0;

            if (sock->window.reno_state == RENO_SLOW_START) {
                // Slow Start: CWND augmente de 1 MSS pour chaque nouvel ACK
                sock->window.congestion_window += MSS;

                // Transition: Si CWND atteint ssthresh
                if (sock->window.congestion_window >= sock->window.ssthresh) {
                    sock->window.reno_state = RENO_CONGESTION_AVOIDANCE;
                    debug_printf("Transition to CONGESTION AVOIDANCE. cwnd=%d\n", sock->window.congestion_window);
                }
            }
            else if (sock->window.reno_state == RENO_CONGESTION_AVOIDANCE) {
                // Congestion Avoidance: CWND augmente de (MSS^2 / CWND)
                sock->window.congestion_window += (MSS * MSS) / sock->window.congestion_window;
            }
            else if (sock->window.reno_state == RENO_FAST_RECOVERY) {
                // Fast Recovery: Un nouvel ACK indique que le paquet perdu a été récupéré.
                sock->window.congestion_window = sock->window.ssthresh; // Retour à ssthresh
                sock->window.reno_state = RENO_CONGESTION_AVOIDANCE;
                debug_printf("Transition to CONGESTION AVOIDANCE (Fast Recovery Exit). cwnd=%d\n", sock->window.congestion_window);
            }
        }

        // 2. Vérifier si l'ACK est un duplicata (Fast Retransmit)
        else if (ack == old_send_base) { // ACK DUPLIQUÉ
            sock->window.dup_ack_count++;

            if (sock->window.reno_state == RENO_FAST_RECOVERY) {
                // En Fast Recovery, chaque ACK dupliqué augmente CWND de 1 MSS
                sock->window.congestion_window += MSS;
                debug_printf("Fast Recovery: Duplicate ACK. cwnd=%d\n", sock->window.congestion_window);
            }
            else if (sock->window.dup_ack_count == 3) {
                // Trois ACKs Dupliqués -> FAST RETRANSMIT (Perte détectée)

                // 1. Ajuster SSTHRESH et CWND
                uint32_t current_cwnd = sock->window.congestion_window;
                sock->window.ssthresh = MAX(current_cwnd / 2, 2 * MSS); // Ne pas aller sous 2 MSS
                sock->window.congestion_window = sock->window.ssthresh + 3 * MSS;
                sock->window.reno_state = RENO_FAST_RECOVERY;

                debug_printf("3 Dup ACKs. Transition to FAST RECOVERY. ssthresh=%d, cwnd=%d\n", sock->window.ssthresh, sock->window.congestion_window);

                // 2. Retransmettre le paquet perdu (SendBase)
                retransmit_send_base(sock);
            }
        }
        // Si l'ACK reçu contenait des données, il faut aussi le traiter comme un paquet de données
        if (!(flags & DATA_FLAG_MASK) && get_payload_len(pkt) == 0) return;
    }

    // ... (Logique de gestion des données pour le récepteur inchangée - GBN) ...
    // Note: La logique de réception du récepteur est toujours en GBN/Stop-and-Wait simplifié.
    if (get_payload_len(pkt) > 0) {
        debug_printf("Received data packet %d, expected %d\n", get_seq(hdr), sock->window.next_seq_expected);

        sock->window.advertised_window = get_advertised_window(hdr);
        add_receive_window(sock, pkt);
        process_receive_window(sock);

        // Envoyer ACK pour le paquet le plus haut en séquence qui a été reçu en ordre.
        debug_printf("Sending ACK packet %d\n", sock->window.next_seq_expected);

        // Assurez-vous d'inclure le RWND actuel (qui est le MAX buffer moins la taille reçue) dans l'ACK
        uint8_t* ack_pkt = create_packet(
            sock->my_port, ntohs(sock->conn.sin_port),
            sock->window.next_seq_num, sock->window.next_seq_expected, // Seq/Ack
            sizeof(foggy_tcp_header_t), sizeof(foggy_tcp_header_t), ACK_FLAG_MASK,
            MAX(MAX_NETWORK_BUFFER - (uint32_t)sock->received_len, MSS), 0,
            NULL, NULL, 0);
        sendto(sock->socket, ack_pkt, sizeof(foggy_tcp_header_t), 0,
            (struct sockaddr*)&(sock->conn), sizeof(sock->conn));
        free(ack_pkt);
    }
}

/**
 * Prépare les données pour l'envoi et déclenche la transmission des paquets dans la fenêtre.
 * @param sock Le socket.
 * @param data Les données à envoyer.
 * @param buf_len La longueur des données.
 */
void send_pkts(foggy_socket_t* sock, uint8_t* data, int buf_len) {
    uint8_t* data_offset = data;

    // 1. Mettre les données dans le buffer d'envoi (tant que buf_len > 0)
    if (buf_len > 0) {
        while (buf_len != 0) {
            uint16_t payload_len = MIN(buf_len, (int)MSS);

            send_window_slot_t slot;
            slot.is_sent = 0;

            // Crée le paquet avec le SeqNum actuel (sock->window.next_seq_num)
            slot.msg = create_packet(
                sock->my_port, ntohs(sock->conn.sin_port),
                sock->window.next_seq_num, sock->window.next_seq_expected, // Seq/Ack
                sizeof(foggy_tcp_header_t), sizeof(foggy_tcp_header_t) + payload_len,
                ACK_FLAG_MASK,
                MAX(MAX_NETWORK_BUFFER - (uint32_t)sock->received_len, MSS), 0, NULL,
                data_offset, payload_len);

            sock->send_window.push_back(slot);

            // Avancer le NextSeqNum pour le paquet suivant
            sock->window.next_seq_num += payload_len;

            buf_len -= payload_len;
            data_offset += payload_len;
        }
    }

    // 2. Transmettre les paquets qui sont autorisés par la fenêtre
    transmit_send_window(sock);
}


/**
 * Logique d'envoi actif : envoie tous les paquets qui sont dans la fenêtre [SendBase, SendBase + WindowSize].
 * @param sock Le socket.
 */
void transmit_send_window(foggy_socket_t* sock) {
    if (sock->send_window.empty()) return;

    // --- MODIFICATION CP3: Utiliser min(CWND, RWND) ---
    // 1. Calculer la taille effective de la fenêtre (Flow Control & Congestion Control)
    uint32_t current_window = MIN(sock->window.congestion_window, sock->window.advertised_window);

    // 2. Déterminer la limite de la fenêtre d'envoi
    uint32_t window_limit = sock->window.send_base + current_window;

    // Boucle pour envoyer tous les paquets qui sont DANS la fenêtre et n'ont pas encore été envoyés.
    std::deque<send_window_slot_t>::iterator it;
    for (it = sock->send_window.begin(); it != sock->send_window.end(); ++it) {
        send_window_slot_t& slot = *it;
        foggy_tcp_header_t* hdr = (foggy_tcp_header_t*)slot.msg;
        uint32_t current_seq = get_seq(hdr);

        // 1. Vérification de la fenêtre : Le paquet est-il dans la fenêtre autorisée ?
        if (before(current_seq, window_limit)) {

            // 2. Vérification de l'envoi : Si le paquet n'a pas été envoyé.
            if (slot.is_sent) {
                continue;
            }

            // ENVOI DU PAQUET
            debug_printf("Sending packet %d %d\n", current_seq, current_seq + get_payload_len(slot.msg));
            slot.is_sent = 1;
            sendto(sock->socket, slot.msg, get_plen(hdr), 0,
                (struct sockaddr*)&(sock->conn), sizeof(sock->conn));

            // 3. Gestion du Timer : Si c'est le paquet de base, démarrer/redémarrer le timer.
            if (current_seq == sock->window.send_base) {
                start_retransmit_timer(sock);
            }
        }
        else {
            // Le reste des paquets est hors de la fenêtre (au-delà de la limite).
            break;
        }
    }
}

/**
 * Purge les paquets acquittés et fait avancer la fenêtre d'envoi.
 * @param sock Le socket.
 */
void receive_send_window(foggy_socket_t* sock) {
    uint32_t new_send_base = sock->window.send_base;

    // Boucle pour retirer tous les paquets qui sont entièrement couverts par le nouveau SendBase
    while (!sock->send_window.empty()) {
        send_window_slot_t slot = sock->send_window.front();
        foggy_tcp_header_t* hdr = (foggy_tcp_header_t*)slot.msg;
        uint32_t packet_seq = get_seq(hdr);
        uint16_t payload_len = get_payload_len(slot.msg);

        // Si la fin du paquet (Seq + Longueur) est <= au nouveau SendBase (ACK), il est acquitté.
        if (before_or_equal(packet_seq + payload_len, new_send_base)) {
            // Ce paquet est acquitté, le retirer
            sock->send_window.pop_front();
            free(slot.msg);
        }
        else {
            // Le premier paquet restant n'est pas complètement acquitté.
            break;
        }
    }
}

// ----------------------------------------------------------------------
// Les fonctions suivantes (add_receive_window, process_receive_window) 
// sont la logique du récepteur GBN simplifié (pas de changement pour CP3).
// ----------------------------------------------------------------------

void add_receive_window(foggy_socket_t* sock, uint8_t* pkt) {
    // ... (Logique Stop-and-wait temporaire inchangée)
    foggy_tcp_header_t* hdr = (foggy_tcp_header_t*)pkt;

    // Stop-and-wait implementation
    receive_window_slot_t* cur_slot = &(sock->receive_window[0]);
    if (cur_slot->is_used == 0) {
        cur_slot->is_used = 1;
        cur_slot->msg = (uint8_t*)malloc(get_plen(hdr));
        memcpy(cur_slot->msg, pkt, get_plen(hdr));
    }
}

void process_receive_window(foggy_socket_t* sock) {
    // ... (Logique Stop-and-wait temporaire inchangée)
    receive_window_slot_t* cur_slot = &(sock->receive_window[0]);
    if (cur_slot->is_used != 0) {
        foggy_tcp_header_t* hdr = (foggy_tcp_header_t*)cur_slot->msg;

        // GBN Récepteur: Si le paquet n'est pas celui attendu, on le DISCARDE.
        if (get_seq(hdr) != sock->window.next_seq_expected) {
            debug_printf("Discarding out-of-order packet %d, expected %d\n", get_seq(hdr), sock->window.next_seq_expected);
            return;
        }

        // Le paquet est celui attendu (in-order)
        uint16_t payload_len = get_payload_len(cur_slot->msg);
        sock->window.next_seq_expected += payload_len; // Avancer le pointeur ACK

        // Copier vers received_buf
        sock->received_buf = (uint8_t*)
            realloc(sock->received_buf, sock->received_len + payload_len);
        memcpy(sock->received_buf + sock->received_len, get_payload(cur_slot->msg),
            payload_len);
        sock->received_len += payload_len;

        // Libérer le slot
        cur_slot->is_used = 0;
        free(cur_slot->msg);
        cur_slot->msg = NULL;
    }
}