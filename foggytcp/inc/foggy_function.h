/* Copyright (C) 2024 Hong Kong University of Science and Technology

This repository is used for the Computer Networks (ELEC 3120) 
course taught at Hong Kong University of Science and Technology. 

No part of the project may be copied and/or distributed without 
the express permission of the course staff. Everyone is prohibited 
from releasing their forks in any public places. */

#include "foggy_tcp.h"

/**
 * Updates the socket information to represent the newly received packet.
 *
 * In the current stop-and-wait implementation, this function also sends an
 * acknowledgement for the packet.
 *
 * @param sock The socket used for handling packets received.
 * @param pkt The packet data received by the socket.
 */
void on_recv_pkt(foggy_socket_t *sock, uint8_t *pkt);


/**
 * Breaks up the data into packets and sends a single packet at a time.
 *
 * You should most certainly update this function in your implementation.
 *
 * @param sock The socket to use for sending data.
 * @param data The data to be sent.
 * @param buf_len The length of the data being sent.
 */
void send_pkts(foggy_socket_t *sock, uint8_t *data, int buf_len);

/*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<*/

// Dans inc/foggy_function.h
// ... (autres déclarations de fonctions) ...

void retransmit_send_base(foggy_socket_t* sock);

void add_receive_window(foggy_socket_t *sock, uint8_t *pkt);

void process_receive_window(foggy_socket_t *sock);

void transmit_send_window(foggy_socket_t *sock);

void receive_send_window(foggy_socket_t *sock);

// Ajoutez ceci APRES le bloc des déclarations de fonctions existantes

// Constantes pour la Fenêtre Glissante
#define WINDOW_SIZE_DEFAULT 10      // Taille initiale de la fenêtre en nombre de segments
#define RTO_INITIAL 500             // Retransmission Timeout initial en ms (par exemple 500 ms)

// Macros pour la comparaison de numéros de séquence (essentiel pour l'enroulement)
#define SEQ_LT(a, b) ((int32_t)((a) - (b)) < 0)
#define SEQ_LE(a, b) ((int32_t)((a) - (b)) <= 0)
#define after(a, b) SEQ_LT(b, a)
#define before(a, b) SEQ_LT(a, b)
#define before_or_equal(a, b) SEQ_LE(a, b)

// Déclarations des fonctions de gestion du Timer (utilisées par foggy_function.cc)
// Ces fonctions doivent être implémentées dans foggy_backend.cc, mais déclarées ici.
void start_retransmit_timer(foggy_socket_t* sock);
void stop_retransmit_timer(foggy_socket_t* sock);
void on_retransmit_timer(foggy_socket_t* sock);