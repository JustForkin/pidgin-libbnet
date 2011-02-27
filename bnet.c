/** 
 * A Protocol Plugin for Pidgin, allowing emulation of a chat-only client
 * connected to the Battle.net Service.
 * Copyright (C) 2011 Nate Book
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _BNET_C_
#define _BNET_C_

#include "bnet.h"

static void bnet_channel_user_free(BnetChannelUser *bcu)
{
    if (bcu != NULL) {
        if (bcu->username != NULL)
            g_free(bcu->username);
        if (bcu->stats_data != NULL)
            g_free(bcu->stats_data);
        g_free(bcu);
    }
}

static void bnet_friend_info_free(BnetFriendInfo *bfi)
{
    if (bfi != NULL) {
        if (bfi->account != NULL)
            g_free(bfi->account);
        if (bfi->location_name != NULL)
            g_free(bfi->location_name);
        if (bfi->stored_status != NULL)
            g_free(bfi->stored_status);
        g_free(bfi);
    }
}

static void bnet_buddy_free(PurpleBuddy *buddy)
{
    bnet_channel_user_free(buddy->proto_data);
}

static void bnet_connect(PurpleAccount *account, gboolean do_register)
{
    // local vars
    PurpleConnection *gc = NULL;
    BnetConnectionData *bnet = NULL;
    char **userparts = NULL;
    PurpleProxyConnectData *bnls_conn_data = NULL;
    const char *username = purple_account_get_username(account);
    
    // set connection flags
    gc = purple_account_get_connection(account);
    gc->flags |= PURPLE_CONNECTION_NO_BGCOLOR;
    gc->flags |= PURPLE_CONNECTION_AUTO_RESP;
    gc->flags |= PURPLE_CONNECTION_NO_NEWLINES;
    gc->flags |= PURPLE_CONNECTION_NO_FONTSIZE;
    gc->flags |= PURPLE_CONNECTION_NO_URLDESC;
    gc->flags |= PURPLE_CONNECTION_NO_IMAGES;
    
    // check for invalid characters in name and server
    if (strpbrk(username, " \t\v\r\n") != NULL) {
        purple_connection_error_reason (gc,
            PURPLE_CONNECTION_ERROR_INVALID_SETTINGS,
            "Battle.net username or server may not contain whitespace");
        return;
    }

    // create and set up the bnet-specific connection data structure
    gc->proto_data = bnet = g_new0(BnetConnectionData, 1);
    bnet->magic = BNET_UDP_SIG; // for debugging
    bnet->account = account;
    bnet->port = purple_account_get_int(account, "port", BNET_DEFAULT_PORT);
    bnet->bnls_server = g_strdup(purple_account_get_string(account, "bnlsserver", BNET_DEFAULT_BNLSSERVER));
    bnet->bnls_port = BNET_DEFAULT_BNLSPORT;
    bnet->product_id = *((int *)((void *)
        purple_account_get_string(bnet->account, "product", "RATS")));
    if (bnet_is_d2(bnet)) {
        bnet->d2_star = "*";
    } else {
        bnet->d2_star = "";
    }
    bnet->crev_complete = FALSE;
    bnet->is_online = FALSE;
    bnet->lookup_user = NULL;
    bnet->lookup_info = NULL;
    
    bnet->create_if_dne = do_register;
    
    //bnet->action_q = g_queue_new();
    
    // save username and server for this connection
    userparts = g_strsplit(username, "@", 2);
    bnet->username = g_strdup(userparts[0]);
    bnet->server = g_strdup(userparts[1]);
    g_strfreev(userparts);

    // set display name
    purple_connection_set_display_name(gc, bnet->username);
    
    // begin connections
    purple_debug_info("bnet", "Connecting to BNLS %s...\n", bnet->bnls_server);
    if (bnet->create_if_dne) {
        purple_connection_update_progress(gc, "Connecting to BNLS", BNET_STEP_BNLS, BNET_STEP_COUNT);
    }
    bnls_conn_data = purple_proxy_connect(gc, account, bnet->bnls_server, bnet->bnls_port,
                 bnls_login_cb, gc);
    if (bnls_conn_data == NULL)
    {
        purple_connection_error_reason (gc,
            PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
            "Unable to connect to the BNLS server");
        return;
    }
    bnet->sbnls.conn_data = bnls_conn_data;
}

static void bnet_login(PurpleAccount *account)
{
    bnet_connect(account, FALSE);
}

static void bnls_login_cb(gpointer data, gint source, const gchar *error_message)
{
    PurpleConnection *gc = data;
    BnetConnectionData *bnet = gc->proto_data;
    
    if (source < 0) {
        gchar *tmp = g_strdup_printf("Unable to connect to BNLS: %s",
            error_message);
        purple_connection_error_reason (gc,
            PURPLE_CONNECTION_ERROR_NETWORK_ERROR, tmp);
        g_free(tmp);
        return; 
    }
    
    purple_debug_info("bnet", "BNLS connected!\n");

    bnet->sbnls.fd = source;
    
    if (bnls_send_REQUESTVERSIONBYTE(bnet)) {
        bnet->sbnls.inpa = purple_input_add(bnet->sbnls.fd, PURPLE_INPUT_READ, bnls_input_cb, gc);
    }
}

static int bnls_send_CHOOSENLSREVISION(BnetConnectionData *bnet)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    
    pkt = bnet_packet_create(BNET_PACKET_BNLS);
    bnet_packet_insert(pkt, &bnet->nls_revision, BNET_SIZE_DWORD);
    
    ret = bnet_packet_send_bnls(pkt, BNET_BNLS_CHOOSENLSREVISION, bnet->sbnls.fd);
    
    return ret;
}

static int bnls_send_LOGONCHALLENGE(BnetConnectionData *bnet)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    const char *username = bnet->username;
    const char *password = purple_account_get_password(bnet->account);
    
    guint namelen = strlen(username);
    guint passlen = strlen(password);
    
    pkt = bnet_packet_create(BNET_PACKET_BNLS);
    bnet_packet_insert(pkt, username, namelen + 1);
    bnet_packet_insert(pkt, password, passlen + 1);
    
    ret = bnet_packet_send_bnls(pkt, BNET_BNLS_LOGONCHALLENGE, bnet->sbnls.fd);
    
    return ret;
}

static int bnls_send_LOGONPROOF(BnetConnectionData *bnet, char *s_and_B)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    
    pkt = bnet_packet_create(BNET_PACKET_BNLS);
    bnet_packet_insert(pkt, s_and_B, 64);
    
    ret = bnet_packet_send_bnls(pkt, BNET_BNLS_LOGONPROOF, bnet->sbnls.fd);
    
    return ret;
}

static int bnls_send_VERSIONCHECKEX2(BnetConnectionData *bnet,
       guint32 login_type, guint32 server_cookie, guint32 udp_cookie,
       guint64 mpq_ft, char *mpq_fn, char *checksum_formula)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    
    guint32 bnls_flags = 0;
    
    pkt = bnet_packet_create(BNET_PACKET_BNLS);
    bnet_packet_insert(pkt, &bnet->game, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &bnls_flags, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &bnls_flags, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &mpq_ft, BNET_SIZE_FILETIME);
    bnet_packet_insert(pkt, mpq_fn, strlen(mpq_fn) + 1);
    bnet_packet_insert(pkt, checksum_formula, strlen(checksum_formula) + 1);
    
    ret = bnet_packet_send_bnls(pkt, BNET_BNLS_VERSIONCHECKEX2, bnet->sbnls.fd);
    
    return ret;
}

static int bnls_send_REQUESTVERSIONBYTE(BnetConnectionData *bnet)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    BnetGameType game;
    
    guint32 product_id = bnet->product_id;
    
    switch (product_id) {
        default:
        case BNET_PRODUCT_STAR: game = BNET_GAME_TYPE_STAR; break;
        case BNET_PRODUCT_SEXP: game = BNET_GAME_TYPE_SEXP; break;
        case BNET_PRODUCT_W2BN: game = BNET_GAME_TYPE_W2BN; break;
        case BNET_PRODUCT_D2DV: game = BNET_GAME_TYPE_D2DV; break;
        case BNET_PRODUCT_D2XP: game = BNET_GAME_TYPE_D2XP; break;
        case BNET_PRODUCT_JSTR: game = BNET_GAME_TYPE_JSTR; break;
        case BNET_PRODUCT_WAR3: game = BNET_GAME_TYPE_WAR3; break;
        case BNET_PRODUCT_W3XP: game = BNET_GAME_TYPE_W3XP; break;
        case BNET_PRODUCT_DRTL: game = BNET_GAME_TYPE_DRTL; break;
        case BNET_PRODUCT_DSHR: game = BNET_GAME_TYPE_DSHR; break;
        case BNET_PRODUCT_SSHR: game = BNET_GAME_TYPE_SSHR; break;
    }
    
    bnet->game = game;
    
    pkt = bnet_packet_create(BNET_PACKET_BNLS);
    bnet_packet_insert(pkt, &game, BNET_SIZE_DWORD);
    
    ret = bnet_packet_send_bnls(pkt, BNET_BNLS_REQUESTVERSIONBYTE, bnet->sbnls.fd);
    
    return ret;
}

static void bnls_input_cb(gpointer data, gint source, PurpleInputCondition cond)
{
    PurpleConnection *gc = data;
    BnetConnectionData *bnet = NULL;
    int len;
    
    if (gc == NULL) return;
    
    bnet = gc->proto_data;
    
    if (bnet->sbnls.inbuflen < bnet->sbnls.inbufused + BNET_INITIAL_BUFSIZE) {
        bnet->sbnls.inbuflen += BNET_INITIAL_BUFSIZE;
        bnet->sbnls.inbuf = g_realloc(bnet->sbnls.inbuf, bnet->sbnls.inbuflen);
    }

    len = read(bnet->sbnls.fd, bnet->sbnls.inbuf + bnet->sbnls.inbufused, BNET_INITIAL_BUFSIZE - 1);
    if (len < 0 && errno == EAGAIN) {
        return;
    } else if (len < 0) {
        purple_input_remove(bnet->sbnls.inpa);
        if (bnet->crev_complete == FALSE) {
            gchar *tmp = g_strdup_printf("Lost connection with BNLS server: %s\n",
                    g_strerror(errno));
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_NETWORK_ERROR, tmp);
            g_free(tmp);
        } else {
            close(bnet->sbnls.fd);
        }
        purple_debug_info("bnet", "BNLS disconnected.\n");
        return;
    } else if (len == 0) {
        purple_input_remove(bnet->sbnls.inpa);
        if (bnet->crev_complete == FALSE) {
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
                "BNLS server closed the connection\n");
        } else {
            close(bnet->sbnls.fd);
        }
        purple_debug_info("bnet", "BNLS disconnected.\n");
        return;
    }

    bnls_read_input(bnet, len);
}

static void bnls_read_input(BnetConnectionData *bnet, int len)
{
    guint8 *this_start;
    guint8 this_id;
    guint16 this_len;
    guint16 inbuftouse = 0;

    bnet->account->gc->last_received = time(NULL);
    bnet->sbnls.inbufused += len;

    this_start = bnet->sbnls.inbuf;
    
    while (this_start + 3 <= bnet->sbnls.inbuf + bnet->sbnls.inbufused)
    {
        this_id = *(this_start + 2);
        this_len = *((guint16 *)(void *)(this_start + 0));
        inbuftouse += this_len;
        if (inbuftouse <= bnet->sbnls.inbufused) {
            bnls_parse_packet(bnet, this_id, this_start, this_len);
            this_start += this_len;
        } else break;
    }
    
    if (this_start != bnet->sbnls.inbuf + bnet->sbnls.inbufused) {
        bnet->sbnls.inbufused -= (this_start - bnet->sbnls.inbuf);
        memmove(bnet->sbnls.inbuf, this_start, bnet->sbnls.inbufused);
    } else {
        bnet->sbnls.inbufused = 0;
    }
}

static void bnls_recv_CHOOSENLSREVISION(BnetConnectionData *bnet, BnetPacket *pkt)
{
    gboolean result = bnet_packet_read_dword(pkt);
    
    if (result) {
        bnls_send_LOGONCHALLENGE(bnet);
    }
}

static void bnls_recv_LOGONCHALLENGE(BnetConnectionData *bnet, BnetPacket *pkt)
{
    char *A = (char *)bnet_packet_read(pkt, 32);
    
    bnet_send_AUTH_ACCOUNTLOGON(bnet, A);
    
    g_free(A);
}

static void bnls_recv_LOGONPROOF(BnetConnectionData *bnet, BnetPacket *pkt)
{
    char *M1 = (char *)bnet_packet_read(pkt, 20);
    
    bnet_send_AUTH_ACCOUNTLOGONPROOF(bnet, M1);
    
    g_free(M1);
}

static void bnls_recv_REQUESTVERSIONBYTE(BnetConnectionData *bnet, BnetPacket *pkt)
{
    // store version byte
    guint32 product_id = bnet_packet_read_dword(pkt);
    PurpleAccount *account = bnet->account;
    PurpleConnection *gc = account->gc;
    PurpleProxyConnectData *conn_data;
    
    if (product_id != 0) {
        guint32 version_code = bnet_packet_read_dword(pkt);
        bnet->version_code = version_code;
    }
    
    // connect to bnet
    purple_debug_info("bnet", "Connecting to %s...\n", bnet->server);
    if (bnet->create_if_dne) {
        purple_connection_update_progress(gc, "Connecting to Battle.net", BNET_STEP_CONNECTING, BNET_STEP_COUNT);
    }
    conn_data = purple_proxy_connect(gc, account, bnet->server, bnet->port,
                 bnet_login_cb, gc);
    if (conn_data == NULL)
    {
        purple_connection_error_reason (gc,
            PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
            "Unable to connect");
        return;
    }
    bnet->sbnet.conn_data = conn_data;
}

static void bnls_recv_VERSIONCHECKEX2(BnetConnectionData *bnet, BnetPacket *pkt)
{
    //
    guint32 success = bnet_packet_read_dword(pkt);
    guint32 exe_version, exe_checksum, cookie, version_code;
    char *exe_info;
    
    bnet->crev_complete = TRUE;
    
    if (success == TRUE) {
        exe_version = bnet_packet_read_dword(pkt);
        exe_checksum = bnet_packet_read_dword(pkt);
        exe_info = bnet_packet_read_cstring(pkt);
        cookie = bnet_packet_read_dword(pkt);
        version_code = bnet_packet_read_dword(pkt);
        bnet->version_code = version_code;
        bnet_send_AUTH_CHECK(bnet,
            exe_version, exe_checksum, exe_info);
        
        g_free(exe_info);
    }
}

static void bnls_parse_packet(BnetConnectionData *bnet, guint8 packet_id, guint8 *packet_start, guint16 packet_len)
{
    BnetPacket *pkt = NULL;
    
    purple_debug_misc("bnet", "BNLS S>C 0x%02x: length %d\n", packet_id, packet_len);
    
    pkt = bnet_packet_refer_bnls(packet_start, packet_len);
    
    switch (packet_id) {
        case BNET_BNLS_LOGONCHALLENGE:
            bnls_recv_LOGONCHALLENGE(bnet, pkt);
            break;
        case BNET_BNLS_LOGONPROOF:
            bnls_recv_LOGONPROOF(bnet, pkt);
            break;
        case BNET_BNLS_REQUESTVERSIONBYTE:
            bnls_recv_REQUESTVERSIONBYTE(bnet, pkt);
            break;
        case BNET_BNLS_VERSIONCHECKEX2:
            bnls_recv_VERSIONCHECKEX2(bnet, pkt);
            break;
        case BNET_BNLS_CHOOSENLSREVISION:
            bnls_recv_CHOOSENLSREVISION(bnet, pkt);
            break;
        default:
        {
            // unhandled
            purple_debug_warning("bnet", "Received unhandled BNLS packet 0x%02x, length %d\n", packet_id, packet_len);
            break;
        }
    }
    
    bnet_packet_free(pkt);
}

static void bnet_login_cb(gpointer data, gint source, const gchar *error_message)
{
    PurpleConnection *gc = data;
    BnetConnectionData *bnet = gc->proto_data;

    if (source < 0) {
        gchar *tmp = g_strdup_printf("Unable to connect: %s",
            error_message);
        purple_connection_error_reason (gc,
            PURPLE_CONNECTION_ERROR_NETWORK_ERROR, tmp);
        g_free(tmp);
        return; 
    }
    
    purple_debug_info("bnet", "Connected!\n");
    if (bnet->create_if_dne) {
        purple_connection_update_progress(gc, "Checking product key and version", BNET_STEP_CREV, BNET_STEP_COUNT);
    }
    
    bnet->sbnet.fd = source;
    
    if (bnet_protocol_begin(bnet)) {
        bnet->sbnet.inpa = gc->inpa = purple_input_add(bnet->sbnet.fd, PURPLE_INPUT_READ, bnet_input_cb, gc);
    }
}

static gboolean bnet_protocol_begin(BnetConnectionData *bnet)
{
    if (bnet_send_protocol_byte(bnet, 0x01) < 0) {
        return FALSE;
    }
    
    if (bnet_send_AUTH_INFO(bnet) < 0) {
        return FALSE;
    }
    
    return TRUE;
}

static int bnet_send_protocol_byte(BnetConnectionData *bnet, int byte)
{
    int ret = write(bnet->sbnet.fd, &byte, 1);
    
    return ret;
}

static int bnet_send_NULL(BnetConnectionData *bnet)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    
    ret = bnet_packet_send(pkt, BNET_SID_NULL, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_ENTERCHAT(BnetConnectionData *bnet)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    guint16 zero = 0;
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    bnet_packet_insert(pkt, bnet->username, strlen(bnet->username) + 1);
    bnet_packet_insert(pkt, &zero, BNET_SIZE_BYTE);
    
    ret = bnet_packet_send(pkt, BNET_SID_ENTERCHAT, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_GETCHANNELLIST(BnetConnectionData *bnet)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    bnet_packet_insert(pkt, &bnet->product_id, BNET_SIZE_DWORD);
    
    ret = bnet_packet_send(pkt, BNET_SID_GETCHANNELLIST, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_JOINCHANNEL(BnetConnectionData *bnet,
           BnetChannelJoinFlags channel_flags, char *channel)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    guint32 chflags = (guint32)channel_flags;
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    bnet_packet_insert(pkt, &chflags, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, channel, strlen(channel) + 1);
    
    ret = bnet_packet_send(pkt, BNET_SID_JOINCHANNEL, bnet->sbnet.fd);
    
    return ret;
}

/*static int bnet_queue_CHATCOMMAND(BnetConnectionData *bnet, PurpleConversation *conv,
        BnetCommandID cmd, BnetQueueFunc cb, const char *command)
{
    BnetQueueElement *qel = g_new0(BnetQueueElement, 1);
    
    qel->conv = conv;
    qel->cmd = cmd;
    qel->pkt_id = BNET_SID_CHATCOMMAND;
    qel->pkt = bnet_packet_create(BNET_PACKET_BNCS);
    qel->pkt_response = BNET_SID_CHATEVENT;
    qel->cookie = 0;
    qel->cb = cb;
    
    bnet_packet_insert(qel->pkt, command, strlen(command) + 1);
    
    bnet_queue(qel);
}*/

static int bnet_send_CHATCOMMAND(BnetConnectionData *bnet, const char *command)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    bnet_packet_insert(pkt, command, strlen(command) + 1);
    
    ret = bnet_packet_send(pkt, BNET_SID_CHATCOMMAND, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_LEAVECHAT(BnetConnectionData *bnet)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    
    ret = bnet_packet_send(pkt, BNET_SID_LEAVECHAT, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_LOGONRESPONSE2(BnetConnectionData *bnet)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    sha1_context sha;
    guint8 h1[SHA1_HASH_SIZE], h2[SHA1_HASH_SIZE];
    const char *username = bnet->username;
    const char *password = purple_account_get_password(bnet->account);
    
    guint namelen = strlen(username);
    guint passlen = strlen(password);
    
    sha.version = xSHA1;
    sha1_reset(&sha);
    sha1_input(&sha, (guint8 *)password, passlen);
    sha1_digest(&sha, h1);
    sha1_reset(&sha);
    sha1_input(&sha, (guint8 *)&bnet->client_cookie, BNET_SIZE_DWORD);
    sha1_input(&sha, (guint8 *)&bnet->server_cookie, BNET_SIZE_DWORD);
    sha1_input(&sha, h1, SHA1_HASH_SIZE);
    sha1_digest(&sha, h2);
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    bnet_packet_insert(pkt, &bnet->client_cookie, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &bnet->server_cookie, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, h2, SHA1_HASH_SIZE);
    bnet_packet_insert(pkt, username, namelen + 1);
    
    ret = bnet_packet_send(pkt, BNET_SID_LOGONRESPONSE2, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_CREATEACCOUNT2(BnetConnectionData *bnet)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    sha1_context sha;
    guint8 h1[SHA1_HASH_SIZE];
    const char *username = bnet->username;
    const char *password = purple_account_get_password(bnet->account);
    
    guint namelen = strlen(username);
    guint passlen = strlen(password);
    
    sha.version = xSHA1;
    sha1_reset(&sha);
    sha1_input(&sha, (const guint8 *)password, passlen);
    sha1_digest(&sha, h1);
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    bnet_packet_insert(pkt, h1, SHA1_HASH_SIZE);
    bnet_packet_insert(pkt, username, namelen + 1);
    
    ret = bnet_packet_send(pkt, BNET_SID_CREATEACCOUNT2, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_PING(BnetConnectionData *bnet, guint32 cookie)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    bnet_packet_insert(pkt, &cookie, BNET_SIZE_DWORD);
    
    ret = bnet_packet_send(pkt, BNET_SID_PING, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_READUSERDATA(BnetConnectionData *bnet,
    int request_cookie, const char *username, char **keys)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    int account_count = 1;
    int key_count = g_strv_length(keys);
    int i;
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    bnet_packet_insert(pkt, &account_count, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &key_count, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &request_cookie, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, username, strlen(username) + 1);
    for (i = 0; i < key_count; i++)
        bnet_packet_insert(pkt, keys[i], strlen(keys[i]) + 1);
    
    ret = bnet_packet_send(pkt, BNET_SID_READUSERDATA, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_WRITEUSERDATA(BnetConnectionData *bnet,
    const char *sex, const char *age, const char *location, const char *description)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    int account_count = 1;
    int key_count = 4;
    char *k_sex = "profile\\sex";
    char *k_age = "profile\\age";
    char *k_location = "profile\\location";
    char *k_description = "profile\\description";
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    bnet_packet_insert(pkt, &account_count, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &key_count, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, bnet->username, strlen(bnet->username) + 1);
    bnet_packet_insert(pkt, k_sex, strlen(k_sex) + 1);
    bnet_packet_insert(pkt, k_age, strlen(k_age) + 1);
    bnet_packet_insert(pkt, k_location, strlen(k_location) + 1);
    bnet_packet_insert(pkt, k_description, strlen(k_description) + 1);
    bnet_packet_insert(pkt, sex, strlen(sex) + 1);
    bnet_packet_insert(pkt, age, strlen(age) + 1);
    bnet_packet_insert(pkt, location, strlen(location) + 1);
    bnet_packet_insert(pkt, description, strlen(description) + 1);
    
    ret = bnet_packet_send(pkt, BNET_SID_WRITEUSERDATA, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_WRITEUSERDATA_2(BnetConnectionData *bnet,
    const char *key, const char *val)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    int account_count = 1;
    int key_count = 1;
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    bnet_packet_insert(pkt, &account_count, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &key_count, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, bnet->username, strlen(bnet->username) + 1);
    bnet_packet_insert(pkt, key, strlen(key) + 1);
    bnet_packet_insert(pkt, val, strlen(val) + 1);
    
    ret = bnet_packet_send(pkt, BNET_SID_WRITEUSERDATA, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_AUTH_INFO(BnetConnectionData *bnet)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    
    guint32 protocol_id = BNET_PROTOCOL_ID;
    guint32 platform_id = BNET_PLATFORM_IX86;
    guint32 product_id = bnet->product_id;
    guint32 version_code = bnet->version_code;
    guint32 product_lang = 1033;
    guint32 local_ip = 0;
    guint32 tz_bias = 0;
    guint32 mpq_lang = 1033;
    guint32 system_lang = 1033;
    char *country_abbr = "USA";
    char *country = "United States";
    
    const char *c_local_ip = purple_network_get_local_system_ip(bnet->sbnet.fd);
    local_ip = *((guint32 *)(void *)(purple_network_ip_atoi(c_local_ip)));
    
    tz_bias = (guint32)(get_tz_bias() / 60.0f);
    
    purple_debug_info("bnet", "local ip %08x\n", local_ip);
    purple_debug_info("bnet", "tz bias %d\n", tz_bias);
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    bnet_packet_insert(pkt, &protocol_id, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &platform_id, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &product_id, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &version_code, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &product_lang, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &local_ip, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &tz_bias, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &mpq_lang, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &system_lang, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, country_abbr, strlen(country_abbr) + 1);
    bnet_packet_insert(pkt, country, strlen(country) + 1);
    
    //purple_debug_info("bnet", "send: \n%s\n", bnet_packet_get_all_data(buf));
     
    ret = bnet_packet_send(pkt, BNET_SID_AUTH_INFO, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_AUTH_CHECK(BnetConnectionData *bnet,
       guint32 exe_version, guint32 exe_checksum, char *exe_info)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    guint32 client_cookie = 0;
    guint32 key_count = 0;
    guint32 key_spawn = 0;
    const char *key_owner = purple_account_get_string(bnet->account, "key_owner", "");
    BnetKey keys[2];
    BnetKey *pkeys = keys;
    int i = 0;
    gboolean keys_are_valid = FALSE;
    
    if (strlen(key_owner) == 0) {
        key_owner = bnet->username;
    }
    
    client_cookie = g_random_int();
    
    bnet->client_cookie = client_cookie;
    
    purple_debug_info("bnet", "server cookie: %08x\n", bnet->server_cookie);
    purple_debug_info("bnet", "client cookie: %08x\n", bnet->client_cookie);
    
    switch (bnet->game) {
        default:
            key_count = 0;
            break;
        case BNET_GAME_TYPE_STAR:
        case BNET_GAME_TYPE_SEXP:
        case BNET_GAME_TYPE_W2BN:
        case BNET_GAME_TYPE_D2DV:
        case BNET_GAME_TYPE_WAR3:
            key_count = 1;
            break;
        case BNET_GAME_TYPE_D2XP:
        case BNET_GAME_TYPE_W3XP:
            key_count = 2;
            break;
    }
    
    pkeys = g_new0(BnetKey, 2);
    
    keys_are_valid = bnet_key_decode(keys, key_count,
        bnet->client_cookie, bnet->server_cookie,
        purple_account_get_string(bnet->account, "key1", ""),
        purple_account_get_string(bnet->account, "key2", ""));
    
    if (!keys_are_valid) {
        char *exp = "";
        char *tmp = NULL;
        if (keys[0].length > 0) {
            // first key valid, second key must not be then
            exp = "expansion ";
        }
        tmp = g_strdup_printf("The provided %sCD-key could not be decoded.", exp);
        purple_connection_error_reason (bnet->account->gc,
            PURPLE_CONNECTION_ERROR_INVALID_SETTINGS,
            tmp);
        g_free(tmp);
        return -1;
    }
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    bnet_packet_insert(pkt, &bnet->client_cookie, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &exe_version, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &exe_checksum, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &key_count, BNET_SIZE_DWORD);
    bnet_packet_insert(pkt, &key_spawn, BNET_SIZE_DWORD);
    for (; i < key_count; i++) {
        bnet_packet_insert(pkt, &keys[i], sizeof(BnetKey));
    }
    bnet_packet_insert(pkt, exe_info, strlen(exe_info) + 1);
    bnet_packet_insert(pkt, key_owner, strlen(key_owner) + 1); 
    
    g_free(pkeys);
    
    ret = bnet_packet_send(pkt, BNET_SID_AUTH_CHECK, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_AUTH_ACCOUNTLOGON(BnetConnectionData *bnet, char *A)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    
    char *username = bnet->username;
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    bnet_packet_insert(pkt, A, 32);
    bnet_packet_insert(pkt, username, strlen(username) + 1);
    
    ret = bnet_packet_send(pkt, BNET_SID_AUTH_ACCOUNTLOGON, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_AUTH_ACCOUNTLOGONPROOF(BnetConnectionData *bnet, char *M1)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    bnet_packet_insert(pkt, M1, 20);
    
    ret = bnet_packet_send(pkt, BNET_SID_AUTH_ACCOUNTLOGONPROOF, bnet->sbnet.fd);
    
    return ret;
}

static int bnet_send_FRIENDSLIST(BnetConnectionData *bnet)
{
    BnetPacket *pkt = NULL;
    int ret = -1;
    
    pkt = bnet_packet_create(BNET_PACKET_BNCS);
    
    ret = bnet_packet_send(pkt, BNET_SID_FRIENDSLIST, bnet->sbnet.fd);
    
    return ret;
}

static void bnet_account_logon(BnetConnectionData *bnet)
{
    if (bnet->nls_revision == 0) {
        bnet_send_LOGONRESPONSE2(bnet);
    } else {
        bnls_send_CHOOSENLSREVISION(bnet);
    }
}

static void bnet_enter_chat(BnetConnectionData *bnet)
{
    bnet_send_ENTERCHAT(bnet);
    bnet_send_GETCHANNELLIST(bnet);
    bnet_send_JOINCHANNEL(bnet,
            (BnetChannelJoinFlags)
            (BNET_CHANNELJOIN_FIRSTJOIN | BNET_CHANNELJOIN_D2FIRST), "Pidgin");
}

static gboolean bnet_keepalive_timer(BnetConnectionData *bnet)
{
    //purple_debug_info("bnet", "keepalive tick\n");
    // keep alive every 30 seconds
    bnet->ka_tick++;
    //purple_debug_info("bnet", "keepalive %d\n", bnet->ka_tick);
    
    // SID_NULL: every 8 minutes
    if ((bnet->ka_tick % 16) == 0) {
        bnet_send_NULL(bnet);
    }
    
    // SID_FRIENDSLIST: every 1 minute
    if ((bnet->ka_tick % 2) == 0) {
        bnet_send_FRIENDSLIST(bnet);
    }
    
    return TRUE;
}

static void bnet_account_register(PurpleAccount *account)
{
    purple_debug_info("bnet", "REGISTER ACCOUNT REQUEST");
    bnet_connect(account, TRUE);
    //PurpleConnection *gc = purple_account_get_connection(account);
    //bnet->create_if_dne = TRUE;
}

static void bnet_account_chpw(PurpleConnection *gc, const char *oldpass, const char *newpass)
{
    BnetConnectionData *bnet = gc->proto_data;
    
    purple_debug_info("bnet", "CHANGE PASSWORD REQUEST");
    bnet->change_pw = TRUE;
    bnet->change_pw_from = g_strdup(oldpass);
    bnet->change_pw_to = g_strdup(newpass);
}

static void bnet_input_cb(gpointer data, gint source, PurpleInputCondition cond)
{
    PurpleConnection *gc = data;
    BnetConnectionData *bnet = gc->proto_data;
    int len;
    
    if (bnet == NULL || bnet->magic != BNET_UDP_SIG) return;
    
    if (bnet->sbnet.inbuflen < bnet->sbnet.inbufused + BNET_INITIAL_BUFSIZE) {
        bnet->sbnet.inbuflen += BNET_INITIAL_BUFSIZE;
        bnet->sbnet.inbuf = g_realloc(bnet->sbnet.inbuf, bnet->sbnet.inbuflen);
    }

    len = read(bnet->sbnet.fd, bnet->sbnet.inbuf + bnet->sbnet.inbufused, BNET_INITIAL_BUFSIZE - 1);
    if (len < 0 && errno == EAGAIN) {
        return;
    } else if (len < 0) {
        gchar *tmp = g_strdup_printf("Lost connection with server: %s\n",
                g_strerror(errno));
        purple_connection_error_reason (gc,
            PURPLE_CONNECTION_ERROR_NETWORK_ERROR, tmp);
        g_free(tmp);
        return;
    } else if (len == 0) {
        purple_connection_error_reason (gc,
            PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
            "Server closed the connection\n");
        return;
    }

    bnet_read_input(bnet, len);
}

static void bnet_read_input(BnetConnectionData *bnet, int len)
{
    guint8 *this_start;
    guint8 this_hdr, this_id;
    guint16 this_len;
    guint16 inbuftouse = 0;

    bnet->account->gc->last_received = time(NULL);
    bnet->sbnet.inbufused += len;

    this_start = bnet->sbnet.inbuf;
    
    while (this_start + 4 <= bnet->sbnet.inbuf + bnet->sbnet.inbufused)
    {
        this_hdr = *(this_start + 0);
        this_id = *(this_start + 1);
        this_len = *((guint16 *)(void *)(this_start + 2));
        inbuftouse += this_len;
        if (this_hdr == BNET_IDENT_FLAG) {
            if (inbuftouse <= bnet->sbnet.inbufused) {
                bnet_parse_packet(bnet, this_id, this_start, this_len);
                this_start += this_len;
            } else break;
        } else break;
    }
    
    if (this_start != bnet->sbnet.inbuf + bnet->sbnet.inbufused) {
        bnet->sbnet.inbufused -= (this_start - bnet->sbnet.inbuf);
        memmove(bnet->sbnet.inbuf, this_start, bnet->sbnet.inbufused);
    } else {
        bnet->sbnet.inbufused = 0;
    }
}

static void bnet_recv_ENTERCHAT(BnetConnectionData *bnet, BnetPacket *pkt)
{
    char *unique_username = bnet_packet_read_cstring(pkt);
    char *statstring = bnet_packet_read_cstring(pkt);
    char *account = bnet_packet_read_cstring(pkt);
    
    bnet->unique_username = unique_username;
    g_free(statstring);
    g_free(account);
}

static void bnet_recv_GETCHANNELLIST(BnetConnectionData *bnet, BnetPacket *pkt)
{
    char *channel;
    
    while (TRUE) {
        channel = bnet_packet_read_cstring(pkt);
        if (channel == NULL || strlen(channel) == 0) break;
        bnet->channel_list = g_list_prepend(bnet->channel_list, channel);
    }
    
    bnet->channel_list = g_list_reverse(bnet->channel_list);
}

static void bnet_recv_CHATEVENT(BnetConnectionData *bnet, BnetPacket *pkt)
{
    BnetChatEventID id;
    BnetChatEventFlags flags;
    gint32 ping;
    char *who;
    char *what;
    
    PurpleConnection *gc = bnet->account->gc;
    PurpleConversation *conv = NULL;
    PurpleConvChat *chat = NULL;
    int chat_id = 0;
    const char *who_n;
    const char *norm;
    
    if (!bnet->is_online) {
        PurplePresence *pres = NULL;
        PurpleStatus *status = NULL;
        
        purple_connection_set_state(gc, PURPLE_CONNECTED);
        bnet->is_online = TRUE;
        bnet->first_join = TRUE;
        
        bnet->ka_handle = purple_timeout_add_seconds(30, (GSourceFunc)bnet_keepalive_timer, bnet);
        
        bnet_send_FRIENDSLIST(bnet);
        
        pres = purple_account_get_presence(bnet->account);
        status = purple_presence_get_active_status(pres);
        bnet_set_status(bnet->account, status);
    }
    
    if (!bnet->first_join && bnet->channel_id != 0)
        conv = purple_find_chat(gc, bnet->channel_id);
    if (conv != NULL)
        chat = purple_conversation_get_chat_data(conv);
    
    id = bnet_packet_read_dword(pkt);
    flags = bnet_packet_read_dword(pkt);
    ping = bnet_packet_read_dword(pkt);
    bnet_packet_read_dword(pkt);
    bnet_packet_read_dword(pkt);
    bnet_packet_read_dword(pkt);
    who = bnet_packet_read_cstring(pkt);
    what = bnet_packet_read_cstring(pkt);
    
    who_n = g_strdup(bnet_d2_normalize(bnet->account, who));
    
    switch (id) {
        case BNET_EID_SHOWUSER:
        {
            GList *li = NULL;
            purple_debug_info("bnet", "USER IN CHANNEL %s %x %dms: %s\n",
                who_n, flags, ping, what);
            
            li = g_list_find_custom(bnet->channel_users, who_n, bnet_channel_user_compare);
            purple_debug_info("bnet", "%d\n", (int)li);
            if (li != NULL) {
                // user stats update
                BnetChannelUser *bcu = li->data;
                
                bcu->flags = flags;
                bcu->ping = ping;
                if (strlen(what) > 0) {
                    bcu->stats_data = g_strdup(what);
                }
                
                if (chat != NULL) {
                    purple_conv_chat_user_set_flags(chat, who_n,
                             bnet_channel_flags_to_prpl_flags(flags));
                }
            } else {
                // new user
                BnetChannelUser *bcu = g_new0(BnetChannelUser, 1);
                
                bcu->username = g_strdup(who_n);
                bcu->stats_data = g_strdup(what);
                bcu->flags = flags;
                bcu->ping = ping;
                bcu->hidden = FALSE;
                bnet->channel_users = g_list_append(bnet->channel_users, bcu);
                
                if (chat != NULL) {
                    purple_conv_chat_add_user(chat, who_n,
                             bnet_channel_message_parse(bcu->stats_data, flags, ping),
                             bnet_channel_flags_to_prpl_flags(flags), FALSE);
                }
            }
            break;
        }
        case BNET_EID_JOIN:
        {
            BnetChannelUser *bcu;
            
            purple_debug_info("bnet", "USER JOINED %s %x %dms: %s\n",
                who_n, flags, ping, what);
            
            bcu = g_new0(BnetChannelUser, 1);
            bcu->username = g_strdup(who_n);
            bcu->stats_data = g_strdup(what);
            bcu->flags = flags;
            bcu->ping = ping;
            bcu->hidden = FALSE;
            bnet->channel_users = g_list_append(bnet->channel_users, bcu);
            
            if (chat != NULL) {
                purple_conv_chat_add_user(chat, who_n,
                         bnet_channel_message_parse(bcu->stats_data, flags, ping),
                         bnet_channel_flags_to_prpl_flags(flags), TRUE);
            }
            break;
        }
        case BNET_EID_LEAVE:
            purple_debug_info("bnet", "USER PARTED %s %x %dms: %s\n",
                who_n, flags, ping, what);
            
            if (chat != NULL) {
                GList *li = g_list_find_custom(bnet->channel_users, who_n, bnet_channel_user_compare);
                if (li != NULL) {
                    bnet->channel_users = g_list_delete_link(bnet->channel_users, li);
                }
                
                purple_conv_chat_remove_user(chat, who_n, NULL);
            }
            break;
        case BNET_EID_WHISPER:
        {
            gboolean prpl_level_ignore = FALSE;
            
            purple_debug_info("bnet", "USER WHISPER %s %x %dms: %s\n",
                who_n, flags, ping, what);
            
            if (strlen(what) > 0) {
                GError *e = NULL;
                GMatchInfo *mi = NULL;
                GRegex *regex = NULL;
                
                //////////////////////////
                // MUTUAL FRIEND STATUS //
                char *regex_str = g_strdup_printf("Your friend %s (?:has entered Battle\\.net|has exited Battle\\.net|entered a (?:.+) game called (?:.+))\\.", g_regex_escape_string(who_n, -1));
                    
                regex = g_regex_new(regex_str, 0, 0, &e);
                
                if (regex == NULL) {
                    purple_debug_warning("bnet", "regex create failed: %s\n", e->message);
                } else if (g_regex_match(regex, what, 0, &mi) &&
                           purple_account_get_bool(bnet->account, "hidemutual", TRUE)) {
                    prpl_level_ignore = TRUE;
                }
                g_match_info_free(mi);
                g_regex_unref(regex);
            }
            
            if (!prpl_level_ignore) {
                serv_got_im(gc, who_n, purple_markup_escape_text(what, strlen(what)),
                    PURPLE_MESSAGE_RECV, time(NULL));
            }
            
            //if (bnet->is_away) {
                // our "auto-response" is sent by BNET if we are away
                // but we don't see it, so lets just show it anyway
                // because we can.
                //serv_got_im(gc, bnet->unique_username, bnet->away_msg,
                //        PURPLE_MESSAGE_AUTO_RESP | PURPLE_MESSAGE_RECV, time(NULL));
                // isn't working as intended >:/
            //}
            break;
        }
        case BNET_EID_TALK:
            purple_debug_info("bnet", "USER TALK %s %x %dms: %s\n",
                who_n, flags, ping, what);
                
            serv_got_chat_in(gc, bnet->channel_id, who_n, PURPLE_MESSAGE_RECV,
                purple_markup_escape_text(what, strlen(what)), time(NULL));
            break;
        case BNET_EID_BROADCAST:
            purple_debug_info("bnet", "BROADCAST %s %x %dms: %s\n",
                who, flags, ping, what);
            break;
        case BNET_EID_CHANNEL:
        {
            gboolean this_firstjoin = FALSE;
            
            purple_debug_info("bnet", "JOIN CHANNEL %s %x %dms: %s\n",
                who, flags, ping, what);
            
            if (bnet->first_join) {
                bnet->first_join = FALSE;
                this_firstjoin = TRUE;
            } else if (bnet->channel_id != 0) {
                if (chat != NULL) {    
                    purple_conv_chat_write(chat, "Battle.net", "You have left this chat. Battle.net only allows being in one channel at any time.", PURPLE_MESSAGE_SYSTEM, time(NULL));
                }
                serv_got_chat_left(gc, bnet->channel_id);
            }
            
            if (bnet->channel_users != NULL) {
                //g_list_free_full(bnet->channel_users, bnet_channel_user_free);
                g_list_free(bnet->channel_users);
                bnet->channel_users = NULL;
            }
            
            norm = bnet_normalize(bnet->account, what);
            chat_id = g_str_hash(norm);
            
            // TODO: get rid of this dumb code
            if (strlen(norm) >= 6 &&
                norm[0] == 'c' && norm[1] == 'l' && norm[2] == 'a' &&
                norm[3] == 'n' && norm[4] == ' ') {
                this_firstjoin = FALSE;
            }
            
            bnet->channel_id = chat_id;
            bnet->channel_name = g_strdup(what);
            bnet->channel_flags = flags;
            if (!this_firstjoin) {
                serv_got_joined_chat(gc, chat_id, what);
            }
            break;
        }
        case BNET_EID_USERFLAGS:
            purple_debug_info("bnet", "USER FLAG UPDATE %s %x %dms: %s\n",
                who_n, flags, ping, what);
            
            if (chat != NULL) {
                GList *li = g_list_find_custom(bnet->channel_users, who_n, bnet_channel_user_compare);
                if (li != NULL) {
                    BnetChannelUser *bcu = li->data;
                    bcu->flags = flags;
                    bcu->ping = ping;
                    if (strlen(what) > 0) {
                        bcu->stats_data = g_strdup(what);
                    }
                }
                
                purple_conv_chat_user_set_flags(chat, who_n,
                         bnet_channel_flags_to_prpl_flags(flags));
            }
            break;
        case BNET_EID_WHISPERSENT:
            purple_debug_info("bnet", "YOU WHISPER %s %x %dms: %s\n",
                who_n, flags, ping, what);
            
            if (bnet->last_sent_to != NULL) {
                bnet->awaiting_whisper_confirm = FALSE;
            }
            //serv_got_im(gc, who_n, what, PURPLE_MESSAGE_SEND, time(NULL));
            break;
        case BNET_EID_CHANNELFULL:
            purple_debug_info("bnet", "CHANNEL IS FULL %s %x %dms: %s\n",
                who, flags, ping, what);
                
            purple_serv_got_join_chat_failed(gc, bnet->join_attempt);
            break;
        case BNET_EID_CHANNELDOESNOTEXIST:
            purple_debug_info("bnet", "CHANNEL DOES NOT EXIST %s %x %dms: %s\n",
                who, flags, ping, what);
                
            purple_serv_got_join_chat_failed(gc, bnet->join_attempt);
            break;
        case BNET_EID_CHANNELRESTRICTED:
            purple_debug_info("bnet", "CHANNEL IS RESTRICTED %s %x %dms: %s\n",
                who, flags, ping, what);
                
            purple_serv_got_join_chat_failed(gc, bnet->join_attempt);
            break;
        case BNET_EID_INFO:
        {
            gboolean handled = FALSE;
            
            purple_debug_info("bnet", "BNET INFO %s %x %dms: %s\n",
                who, flags, ping, what);
            
            if (strlen(what) > 0) {
                GError *e = NULL;
                GMatchInfo *mi = NULL;
                GRegex *regex = NULL;
                
                ////////////////////
                // WHOIS RESPONSE //
                regex = g_regex_new(
                    "(?:You are |)(\\S+)(?:,| is) using (.+) in (.+)\\.", 0, 0, &e);
                
                if (regex == NULL) {
                    purple_debug_warning("bnet", "regex create failed: %s\n", e->message);
                } else if (g_regex_match(regex, what, 0, &mi)) {
                    PurpleBuddy *b;
                    
                    gchar *whois_user = g_match_info_fetch(mi, 1);
                    gchar *whois_product = g_match_info_fetch(mi, 2);
                    gchar *whois_location = g_match_info_fetch(mi, 3);
                    const gchar *whois_user_n = NULL;
                    
                    whois_user_n = bnet_d2_normalize(bnet->account, whois_user);
                    
                    b = purple_find_buddy(bnet->account, whois_user_n);
                    if (b != NULL) {
                        BnetFriendInfo *bfi = purple_buddy_get_protocol_data(b);
                        if (bfi != NULL) {
                            if (bfi->automated_lookup) {
                                handled = TRUE;
                            }
                        }
                    }
                    
                    if (!handled && bnet->lookup_user != NULL) {
                        handled = TRUE;
                        
                        if (!bnet->lookup_info) {
                            bnet->lookup_info = purple_notify_user_info_new();
                        } else {
                            purple_notify_user_info_add_section_break(bnet->lookup_info);
                        }
                        
                        purple_notify_user_info_add_pair(bnet->lookup_info, "Current location", whois_location);
                        purple_notify_user_info_add_pair(bnet->lookup_info, "Current product", whois_product);
                        
                        purple_notify_userinfo(bnet->account->gc, whois_user_n,
                            bnet->lookup_info, bnet_whois_complete, bnet);
                    }
                    
                    g_free(whois_user);
                    g_free(whois_product);
                    g_free(whois_location);
                }
                g_match_info_free(mi);
                g_regex_unref(regex);
                
                e = NULL; mi = NULL;
                
                /////////////////////////
                // WHOIS AWAY RESPONSE //
                ///////////////////////////
                // WHISPER AWAY RESPONSE //
                regex = g_regex_new(
                    "(?:You are|(\\S+) is) away \\((.+)\\)", 0, 0, &e);
                
                if (regex == NULL) {
                    purple_debug_warning("bnet", "regex create failed: %s\n", e->message);
                } else if (g_regex_match(regex, what, 0, &mi)) {
                    PurpleBuddy *b;
                    
                    gchar *away_user = g_match_info_fetch(mi, 1);
                    gchar *away_msg = g_match_info_fetch(mi, 2);
                    const gchar *away_user_n = NULL;
                    
                    if (strlen(away_user) == 0) {
                        g_free(away_user);
                        away_user = g_strdup(bnet->unique_username);
                    }
                    
                    away_user_n = bnet_d2_normalize(bnet->account, away_user);
                    
                    b = purple_find_buddy(bnet->account, away_user_n);
                    if (b != NULL) {
                        BnetFriendInfo *bfi = purple_buddy_get_protocol_data(b);
                        if (bfi != NULL) {
                            bfi->stored_status = away_msg;
                            if (bfi->automated_lookup) {
                                handled = TRUE;
                                bfi->automated_lookup = FALSE;
                            }
                        }
                        
                        purple_prpl_got_user_status(bnet->account, away_user_n,
                                BNET_STATUS_AWAY, "message", away_msg, NULL);
                    }
                    
                    if (!handled && bnet->lookup_user != NULL) {
                        handled = TRUE;
                        if (!bnet->lookup_info) {
                            bnet->lookup_info = purple_notify_user_info_new();
                        } else {
                            purple_notify_user_info_add_section_break(bnet->lookup_info);
                        }
                        
                        purple_notify_user_info_add_pair(bnet->lookup_info, "Away", away_msg);
                        
                        purple_notify_userinfo(bnet->account->gc,
                            bnet_d2_normalize(bnet->account, away_user_n),
                            bnet->lookup_info, bnet_whois_complete, bnet);
                    }
                    
                    if (!handled && bnet->last_sent_to != NULL) {
                        PurpleConversation *conv = 
                                purple_find_conversation_with_account(
                                    PURPLE_CONV_TYPE_IM, bnet->last_sent_to, bnet->account);
                        if (conv) {
                            PurpleConvIm *im = purple_conversation_get_im_data(conv);
                            if (im) {
                                char *tmp = g_strdup_printf("Away (%s)", away_msg);
                                handled = TRUE;
                                // this is our "auto-response"
                                serv_got_im(gc, away_user_n, tmp,
                                        PURPLE_MESSAGE_AUTO_RESP, time(NULL));
                                g_free(tmp);
                            }
                        }
                        
                        bnet->awaiting_whisper_confirm = FALSE;
                    }
                    
                    g_free(away_user);
                    g_free(away_msg);
                }
                g_match_info_free(mi);
                g_regex_unref(regex);
                
                e = NULL; mi = NULL;
                
                ////////////////////////
                // WHOIS DND RESPONSE //
                regex = g_regex_new(
                    "(?:You are|(\\S+) is) refusing messages \\((.+)\\)", 0, 0, &e);
                
                if (regex == NULL) {
                    purple_debug_warning("bnet", "regex create failed: %s\n", e->message);
                } else if (g_regex_match(regex, what, 0, &mi)) {
                    PurpleBuddy *b;
                    
                    gchar *dnd_user = g_match_info_fetch(mi, 1);
                    gchar *dnd_msg = g_match_info_fetch(mi, 2);
                    const gchar *dnd_user_n = NULL;
                    
                    if (strlen(dnd_user) == 0) {
                        g_free(dnd_user);
                        dnd_user = g_strdup(bnet->unique_username);
                    }
                    
                    dnd_user_n = bnet_d2_normalize(bnet->account, dnd_user);
                    
                    b = purple_find_buddy(bnet->account, dnd_user_n);
                    if (b != NULL) {
                        BnetFriendInfo *bfi = purple_buddy_get_protocol_data(b);
                        if (bfi != NULL) {
                            bfi->stored_status = dnd_msg;
                            if (bfi->automated_lookup) {
                                handled = TRUE;
                                bfi->automated_lookup = FALSE;
                            }
                        }
                        
                        purple_prpl_got_user_status(bnet->account, dnd_user_n,
                                BNET_STATUS_DND, "message", dnd_msg, NULL);
                    }
                    
                    if (!handled && bnet->lookup_user != NULL) {
                        handled = TRUE;
                        if (!bnet->lookup_info) {
                            bnet->lookup_info = purple_notify_user_info_new();
                        } else {
                            purple_notify_user_info_add_section_break(bnet->lookup_info);
                        }
                        
                        purple_notify_user_info_add_pair(bnet->lookup_info, "Do Not Disturb", dnd_msg);
                        
                        purple_notify_userinfo(bnet->account->gc,
                            bnet_d2_normalize(bnet->account, dnd_user_n),
                            bnet->lookup_info, bnet_whois_complete, bnet);
                    }
                    
                    g_free(dnd_user);
                    g_free(dnd_msg);
                }
                g_match_info_free(mi);
                g_regex_unref(regex);
                
                e = NULL; mi = NULL;
                
                ///////////////////
                // AWAY RESPONSE //
                ////////////////////////
                // STILL AWAY WARNING //
                regex = g_regex_new(
                    "You are (still|now|no longer) marked as (?:being |)away\\.", 0, 0, &e);
                
                if (regex == NULL) {
                    purple_debug_warning("bnet", "regex create failed: %s\n", e->message);
                } else if (g_regex_match(regex, what, 0, &mi)) {
                    gchar *away_state_string = g_match_info_fetch(mi, 1);
                    
                    if (strcmp(away_state_string, "still") == 0) {
                        if (bnet->last_sent_to != NULL) {
                            PurpleConversation *conv = 
                                    purple_find_conversation_with_account(
                                        PURPLE_CONV_TYPE_IM, bnet->last_sent_to, bnet->account);
                            if (conv) {
                                PurpleConvIm *im = purple_conversation_get_im_data(conv);
                                if (im) {
                                    handled = TRUE;
                                    purple_conv_im_write(im, "Battle.net", what, PURPLE_MESSAGE_SYSTEM, time(NULL));
                                }
                            }
                        }
                    } else {
                        bnet->is_away = (strcmp(away_state_string, "now") == 0);
                        
                        if (bnet->setting_away_status) {
                            handled = TRUE;
                            bnet->setting_away_status = FALSE;
                        }
                    }
                    
                    g_free(away_state_string);
                }
                g_match_info_free(mi);
                g_regex_unref(regex);
                
                e = NULL; mi = NULL;
                
                //////////////////
                // DND RESPONSE //
                regex = g_regex_new(
                    "Do Not Disturb mode (engaged|cancelled)\\.", 0, 0, &e);
                
                if (regex == NULL) {
                    purple_debug_warning("bnet", "regex create failed: %s\n", e->message);
                } else if (g_regex_match(regex, what, 0, &mi)) {
                    gchar *dnd_state_string = g_match_info_fetch(mi, 1);
                    bnet->is_dnd = (strcmp(dnd_state_string, "engaged") == 0);
                    
                    if (bnet->setting_dnd_status) {
                        handled = TRUE;
                        bnet->setting_dnd_status = FALSE;
                    }
                    
                    g_free(dnd_state_string);
                }
                g_match_info_free(mi);
                g_regex_unref(regex);
                
                e = NULL; mi = NULL;
                
                ///////////////////////
                // WHISPER DND ERROR //
                regex = g_regex_new(
                    "(\\S+) is unavailable \\((.+)\\)", 0, 0, &e);
                
                if (regex == NULL) {
                    purple_debug_warning("bnet", "regex create failed: %s\n", e->message);
                } else if (g_regex_match(regex, what, 0, &mi)) {
                    if (bnet->last_sent_to != NULL) {
                        handled = TRUE;
                        if (!purple_conv_present_error(bnet->last_sent_to, bnet->account, what)) {
                            purple_notify_error(gc, "Do not disturb", what,
                                g_strdup_printf("%s did not receive your whisper.", bnet->last_sent_to));
                        }
                        
                        bnet->awaiting_whisper_confirm = FALSE;
                    }
                }
                g_match_info_free(mi);
                g_regex_unref(regex);
                
                e = NULL; mi = NULL;
                
                ////////////////////////
                // UNHANDLED EID_INFO //
                if (!handled) {
                    if (bnet->last_command_conv != NULL) {
                        PurpleConversation *conv = bnet->last_command_conv;
                        PurpleConvIm *im = purple_conversation_get_im_data(conv);
                        if (im) {
                            purple_conv_im_write(im, "Battle.net", what, PURPLE_MESSAGE_SYSTEM, time(NULL));
                        } else if (chat) {
                            purple_conv_chat_write(chat, "Battle.net", what, PURPLE_MESSAGE_SYSTEM, time(NULL));
                        } else {
                            purple_notify_info(gc, "Information", what, NULL);
                        }
                    } else if (chat) {
                        purple_conv_chat_write(chat, "Battle.net", what, PURPLE_MESSAGE_SYSTEM, time(NULL));
                    } else {
                        bnet->welcome_msgs = g_list_append(bnet->welcome_msgs, what);
                    }
                }
            }
            break;
        }
        case BNET_EID_ERROR:
        {
            gboolean handled = FALSE;
            purple_debug_info("bnet", "BNET ERROR %s %x %dms: %s\n",
                who, flags, ping, what);
            
            ////////////////////////
            // WHISPERS AND WHOIS //
            if (strcmp(what, "That user is not logged on.") == 0) {
                if (bnet->lookup_user != NULL) {
                    handled = TRUE;
                    if (!bnet->lookup_info) {
                        bnet->lookup_info = purple_notify_user_info_new();
                    } else {
                        purple_notify_user_info_add_section_break(bnet->lookup_info);
                    }
                    
                    purple_notify_user_info_add_pair(bnet->lookup_info, "Current location", "offline");
                    
                    purple_notify_userinfo(bnet->account->gc,
                        bnet_d2_normalize(bnet->account, bnet->lookup_user),
                        bnet->lookup_info, bnet_whois_complete, bnet);
                }
                
                if (!handled && bnet->last_sent_to != NULL) {
                    handled = TRUE;
                    if (!purple_conv_present_error(bnet->last_sent_to, bnet->account, what)) {
                        purple_notify_error(gc, "Not logged in", what,
                            g_strdup_printf("%s did not receive your whisper.", bnet->last_sent_to));
                    }
                    
                    bnet->awaiting_whisper_confirm = FALSE;
                }
            }
            
            
            /////////////////////////
            // UNHANDLED EID_ERROR //
            if (!handled) {
                if (bnet->last_command_conv) {
                    PurpleConversation *conv = bnet->last_command_conv;
                    PurpleConvIm *im = purple_conversation_get_im_data(conv);
                    if (im) {
                        purple_conv_im_write(im, "Battle.net", what, PURPLE_MESSAGE_ERROR, time(NULL));
                    } else if (chat) {
                        purple_conv_chat_write(chat, "Battle.net", what, PURPLE_MESSAGE_ERROR, time(NULL));
                    } else {
                        purple_notify_info(gc, "Error", what, NULL);
                    }
                } else if (chat) {
                    purple_conv_chat_write(chat, "Battle.net", what, PURPLE_MESSAGE_ERROR, time(NULL));
                } else {
                    purple_notify_error(gc, "Error", what, NULL);
                }
            }
            break;
        }
        case BNET_EID_EMOTE:
        {
            purple_debug_info("bnet", "USER EMOTE %s %x %dms: %s\n",
                who_n, flags, ping, what);

            serv_got_chat_in(gc, bnet->channel_id, who_n, 
                ((strcmp(bnet->unique_username, who_n) == 0) ?
                PURPLE_MESSAGE_SEND : PURPLE_MESSAGE_RECV),
                g_strdup_printf("/me %s",
                    ((strlen(what) == 0) ? " " :
                    purple_markup_escape_text(what, strlen(what)))),
                time(NULL));
            break;
        }
    }
    
    g_free(who);
    g_free(what);
    g_free(who_n);
}

static void bnet_recv_MESSAGEBOX(BnetConnectionData *bnet, BnetPacket *pkt)
{
    guint32 style = bnet_packet_read_dword(pkt);
    char *text = bnet_packet_read_cstring(pkt);
    char *caption = bnet_packet_read_cstring(pkt);
    
    //PurpleConnection *gc = bnet->account->gc;
    
    if (style & 0x00000010L) { // error
        purple_notify_error(bnet,
            g_strdup_printf("Battle.net error: %s", caption), text, NULL);
    } else if (style & 0x00000030L) { // warning
        purple_notify_warning(bnet,
            g_strdup_printf("Battle.net warning: %s", caption), text, NULL);
    } else { // info, question, or nothing
        purple_notify_info(bnet,
            g_strdup_printf("Battle.net info: %s", caption), text, NULL);
    }
}

static void bnet_recv_PING(BnetConnectionData *bnet, BnetPacket *pkt)
{
    guint32 cookie = bnet_packet_read_dword(pkt);
    
    // echo
    bnet_send_PING(bnet, cookie);
}

static void bnet_recv_READUSERDATA(BnetConnectionData *bnet, BnetPacket *pkt)
{
    // readuserdata
    guint32 key_count, request_cookie/*, account_count*/;
    int i, j;
    /*account_count = */bnet_packet_read_dword(pkt); // always 1
    key_count = bnet_packet_read_dword(pkt);
    request_cookie = bnet_packet_read_dword(pkt);
    
    for (i = 0; i < g_list_length(bnet->userdata_requests); i++) {
        BnetUserDataRequest *req = g_list_nth_data(bnet->userdata_requests, i);
        if (req->cookie == request_cookie) {
            GHashTable *userdata = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
            gboolean showing_lookup_dialog = FALSE;
            
            for (j = 0; j < key_count; j++) {
                g_hash_table_insert(userdata, req->userdata_keys[j],
                            bnet_packet_read_cstring(pkt));
            }
            
            if (req->request_type & BNET_READUSERDATA_REQUEST_PROFILE) {
                if (bnet->writing_profile) {
                    char *psex = g_hash_table_lookup(userdata, "profile\\sex");
                    char *page = g_hash_table_lookup(userdata, "profile\\age");
                    char *ploc = g_hash_table_lookup(userdata, "profile\\location");
                    char *pdescr = g_hash_table_lookup(userdata, "profile\\description");
                    bnet_profile_show_write_dialog(bnet, psex, page, ploc, pdescr);
                } else if (bnet->lookup_user != NULL) {
                    int section_count = 0;
                    char *pstr;
                    showing_lookup_dialog = TRUE;
                    
                    if (!bnet->lookup_info) {
                        bnet->lookup_info = purple_notify_user_info_new();
                    } else {
                        purple_notify_user_info_add_section_break(bnet->lookup_info);
                    }
                    
                    // profile\sex
                    pstr = g_hash_table_lookup(userdata, "profile\\sex");
                    if (pstr != NULL && strlen(pstr) > 0) {
                        purple_notify_user_info_add_pair(bnet->lookup_info, "Profile sex", 
                            purple_markup_escape_text(pstr, strlen(pstr)));
                        section_count++;
                    }
                    
                    // profile\age
                    pstr = g_hash_table_lookup(userdata, "profile\\age");
                    if (pstr != NULL && strlen(pstr) > 0) {
                        purple_notify_user_info_add_pair(bnet->lookup_info, "Profile age", 
                            purple_markup_escape_text(pstr, strlen(pstr)));
                        section_count++;
                    }
                    
                    // profile\location
                    pstr = g_hash_table_lookup(userdata, "profile\\location");
                    if (pstr != NULL && strlen(pstr) > 0) {
                        purple_notify_user_info_add_pair(bnet->lookup_info, "Profile location", 
                            purple_markup_escape_text(pstr, strlen(pstr)));
                        section_count++;
                    }
                    
                    // profile\description
                    pstr = g_hash_table_lookup(userdata, "profile\\description");
                    if (pstr != NULL && strlen(pstr) > 0) {
                        purple_notify_user_info_add_pair(bnet->lookup_info, "Profile description", 
                            purple_markup_escape_text(pstr, strlen(pstr)));
                        section_count++;
                    }
                    
                    if (section_count == 0) {
                        purple_notify_user_info_add_pair(bnet->lookup_info, "Profile", 
                            "No information is stored in this user's profile.");
                    }
                }
            }
            
            if (req->request_type & BNET_READUSERDATA_REQUEST_SYSTEM) {
                if (bnet->lookup_user != NULL) {
                    gboolean is_section = FALSE;
                    char *pstr;
                    showing_lookup_dialog = TRUE;
                    
                    if (!bnet->lookup_info) {
                        bnet->lookup_info = purple_notify_user_info_new();
                    }
                    
                    // System\Account Created
                    pstr = g_hash_table_lookup(userdata, "System\\Account Created");
                    if (pstr != NULL && strlen(pstr) > 0) {
                        if (!is_section) {
                            purple_notify_user_info_add_section_break(bnet->lookup_info);
                            is_section = TRUE;
                        }
                    
                        purple_notify_user_info_add_pair(bnet->lookup_info, "Account creation time", 
                            bnet_format_strftime(pstr));
                    }
                    
                    // System\Last Logoff
                    pstr = g_hash_table_lookup(userdata, "System\\Last Logoff");
                    if (pstr != NULL && strlen(pstr) > 0) {
                        if (!is_section) {
                            purple_notify_user_info_add_section_break(bnet->lookup_info);
                            is_section = TRUE;
                        }
                        
                        purple_notify_user_info_add_pair(bnet->lookup_info, "Last logoff time", 
                            bnet_format_strftime(pstr));
                    }
                    
                    // System\Last Logon
                    pstr = g_hash_table_lookup(userdata, "System\\Last Logon");
                    if (pstr != NULL && strlen(pstr) > 0) {
                        if (!is_section) {
                            purple_notify_user_info_add_section_break(bnet->lookup_info);
                            is_section = TRUE;
                        }
                        
                        purple_notify_user_info_add_pair(bnet->lookup_info, "Last logon time", 
                            bnet_format_strftime(pstr));
                    }
                    
                    // System\Time Logged
                    pstr = g_hash_table_lookup(userdata, "System\\Time Logged");
                    if (pstr != NULL && strlen(pstr) > 0) {
                        if (!is_section) {
                            purple_notify_user_info_add_section_break(bnet->lookup_info);
                            is_section = TRUE;
                        }
                        
                        purple_notify_user_info_add_pair(bnet->lookup_info, "Account time logged", 
                            bnet_format_strsec(pstr));
                    }
                }
            }
            
            if (req->request_type & BNET_READUSERDATA_REQUEST_RECORD) {
                if (bnet->lookup_user != NULL) {
                    gboolean is_section = FALSE;
                    showing_lookup_dialog = TRUE;
                    
                    if (!bnet->lookup_info) {
                        bnet->lookup_info = purple_notify_user_info_new();
                    }
                    
                    for (j = 0; j < 4; j++) {
                        char *zero = "0";
                        char *key;
                        char *wins; char *losses; char *discs; char *lgame; char *lgameres;
                        char *rating; char *hrating; char *rank; char *hrank;
                        char *header_text = NULL;
                        char *product_id = get_product_id_str(req->product);
                        char *product = get_product_name(req->product);
                        
                        switch (j) {
                            case 0: header_text = "Normal"; break;
                            case 1: header_text = "Ladder"; break;
                            case 3: header_text = "IronMan"; break;
                        }
                        
                        key = g_strdup_printf("Record\\%s\\%d\\wins", product_id, j);
                        wins = g_hash_table_lookup(userdata, key);
                        purple_debug_info("bnet", "key: %s  value: %s\n", key, wins);
                        g_free(key);
                        key = g_strdup_printf("Record\\%s\\%d\\losses", product_id, j);
                        losses = g_hash_table_lookup(userdata, key);
                        purple_debug_info("bnet", "key: %s  value: %s\n", key, losses);
                        g_free(key);
                        key = g_strdup_printf("Record\\%s\\%d\\disconnects", product_id, j);
                        discs = g_hash_table_lookup(userdata, key);
                        purple_debug_info("bnet", "key: %s  value: %s\n", key, discs);
                        g_free(key);
                        key = g_strdup_printf("Record\\%s\\%d\\last game", product_id, j);
                        lgame = g_hash_table_lookup(userdata, key);
                        purple_debug_info("bnet", "key: %s  value: %s\n", key, lgame);
                        g_free(key);
                        key = g_strdup_printf("Record\\%s\\%d\\last game result", product_id, j);
                        lgameres = g_hash_table_lookup(userdata, key);
                        purple_debug_info("bnet", "key: %s  value: %s\n", key, lgameres);
                        g_free(key);
                        
                        if (wins != NULL && losses != NULL && discs != NULL &&
                            lgame != NULL && lgameres != NULL) {
                            if (!is_section) {
                                purple_notify_user_info_add_section_break(bnet->lookup_info);
                                is_section = TRUE;
                            }
                            
                            if (strlen(wins) == 0) wins = zero;
                            if (strlen(losses) == 0) losses = zero;
                            if (strlen(discs) == 0) discs = zero;
                            if (strlen(lgame) == 0) {
                                lgame = "never";
                            } else {
                                lgame = g_strdup_printf("%s on %s", lgameres, bnet_format_strftime(lgame));
                            }
                            
                            purple_notify_user_info_add_pair(bnet->lookup_info,
                                g_strdup_printf("%s record for %s", header_text, product), 
                                g_strdup_printf("%s-%s-%s", wins, losses, discs));
                            purple_notify_user_info_add_pair(bnet->lookup_info, "Last game", lgame);
                        }
                        
                        key = g_strdup_printf("Record\\%s\\%d\\rating", product_id, j);
                        rating = g_hash_table_lookup(userdata, key);
                        purple_debug_info("bnet", "key: %s  value: %s\n", key, rating);
                        g_free(key);
                        key = g_strdup_printf("Record\\%s\\%d\\high rating", product_id, j);
                        hrating = g_hash_table_lookup(userdata, key);
                        purple_debug_info("bnet", "key: %s  value: %s\n", key, hrating);
                        g_free(key);
                        key = g_strdup_printf("DynKey\\%s\\%d\\rank", product_id, j);
                        rank = g_hash_table_lookup(userdata, key);
                        purple_debug_info("bnet", "key: %s  value: %s\n", key, rank);
                        g_free(key);
                        key = g_strdup_printf("Record\\%s\\%d\\high rank", product_id, j);
                        hrank = g_hash_table_lookup(userdata, key);
                        purple_debug_info("bnet", "key: %s  value: %s\n", key, hrank);
                        g_free(key);
                        
                        if (rating != NULL && hrating != NULL &&
                            rank != NULL && hrank != NULL) {
                            
                            if (!is_section) {
                                purple_notify_user_info_add_section_break(bnet->lookup_info);
                                is_section = TRUE;
                            }
                            
                            if (strlen(rating) == 0) rating = zero;
                            if (strlen(hrating) == 0) hrating = zero;
                            if (strlen(rank) == 0) rank = zero;
                            if (strlen(hrank) == 0) hrank = zero;
                            
                            purple_notify_user_info_add_pair(bnet->lookup_info, "Rating", 
                                g_strdup_printf("%s (high: %s)", rating, hrating));
                            purple_notify_user_info_add_pair(bnet->lookup_info, "Rank", 
                                g_strdup_printf("%s (high: %s)", rank, hrank));
                        }
                    }
                }
            }
            
            if (showing_lookup_dialog) {
                purple_notify_userinfo(bnet->account->gc,
                    bnet_d2_normalize(bnet->account, bnet->lookup_user),
                    bnet->lookup_info, bnet_whois_complete, bnet);
            }
            
            bnet->userdata_requests = g_list_remove(bnet->userdata_requests, req);
            
            g_hash_table_destroy(userdata);
        }
    }
}

static void bnet_recv_AUTH_INFO(BnetConnectionData *bnet, BnetPacket *pkt)
{
    guint32 login_type = bnet_packet_read_dword(pkt);
    guint32 server_cookie = bnet_packet_read_dword(pkt);
    guint32 udp_cookie = bnet_packet_read_dword(pkt);
    guint64 mpq_ft = bnet_packet_read_qword(pkt);
    char* mpq_fn = bnet_packet_read_cstring(pkt);
    char* checksum_formula = bnet_packet_read_cstring(pkt);
    
    //purple_debug_info("bnet", "mpqfn: %s; chfm: %s\n",
    //    mpq_fn, checksum_formula);
    bnet->nls_revision = login_type;
    bnet->server_cookie = server_cookie;
    bnet->udp_cookie = udp_cookie;
    
    bnls_send_VERSIONCHECKEX2(bnet,
        login_type, server_cookie, udp_cookie, mpq_ft, mpq_fn, checksum_formula);
    
    g_free(mpq_fn);
    g_free(checksum_formula);
}

static void bnet_recv_AUTH_CHECK(BnetConnectionData *bnet, BnetPacket *pkt)
{
    guint32 result = bnet_packet_read_dword(pkt);
    char *extra_info = bnet_packet_read_cstring(pkt);
    
    PurpleConnection *gc = bnet->account->gc;
    
    char *tmp = NULL;
    char *tmpe = NULL;
    char *tmpf = NULL;
    char *tmpkn = NULL;
    
    if (result == BNET_SUCCESS) {
        purple_debug_info("bnet", "Version and key check passed!\n");
        if (bnet->create_if_dne) {
        }
        
        purple_connection_update_progress(gc, "Authenticating", BNET_STEP_LOGON, BNET_STEP_COUNT);
        
        bnet_account_logon(bnet);
        
        g_free(extra_info);
        
        return;
    } else if (result & BNET_AUTH_CHECK_VERERROR_MASK) {
        switch (result & BNET_AUTH_CHECK_ERROR_MASK) {
            case BNET_AUTH_CHECK_VERERROR_INVALID:
                tmp = "Version invalid";
                break;
            case BNET_AUTH_CHECK_VERERROR_OLD:
                tmp = "Old version";
                break;
            case BNET_AUTH_CHECK_VERERROR_NEW:
                tmp = "New version";
                break;
            default:
                tmp = "Version invalid";
                break;
        }
    } else if (result & BNET_AUTH_CHECK_KEYERROR_MASK) {
        guint32 keynum = (result & BNET_AUTH_CHECK_KEYNUMBER_MASK) >> 4;
        switch (result & BNET_AUTH_CHECK_ERROR_MASK) {
            case BNET_AUTH_CHECK_KEYERROR_INVALID:
                tmp = "CD-key invalid";
                break;
            case BNET_AUTH_CHECK_KEYERROR_INUSE:
                tmp = "CD-key is in use";
                break;
            case BNET_AUTH_CHECK_KEYERROR_BANNED:
                tmp = "CD-key is banned";
                break;
            case BNET_AUTH_CHECK_KEYERROR_BADPRODUCT:
                tmp = "CD-key is for another game";
                break;
            default:
                tmp = "CD-key invalid";
                break;
        }
        tmpkn = g_strdup_printf("%s%s", (keynum == 1) ? "Expansion " : "", tmp);
        tmp = tmpkn;
    } else if (result & BNET_AUTH_CHECK_VERCODEERROR_MASK) {
        tmp = "Version code invalid";
    } else {
        tmp = "Authorization failed";
    }
    
    tmpe = g_strdup_printf(" (%s).", extra_info);
    tmpf = g_strdup_printf("%s%s", tmp, strlen(extra_info) > 0 ? tmpe : ".");
    purple_connection_error_reason (gc, PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED, tmpf);
    
    g_free(tmpe);
    g_free(tmpf);
    if (tmpkn) g_free(tmpkn);
    
    //purple_debug_info("bnet", "AUTH_CHECK: 0x%08x %s\n", result, extra_info);
    
    g_free(extra_info);
}

static void bnet_recv_AUTH_ACCOUNTLOGON(BnetConnectionData *bnet, BnetPacket *pkt)
{
    guint32 result = bnet_packet_read_dword(pkt);
    
    PurpleConnection *gc = bnet->account->gc;
    
    switch (result) {
        case BNET_SUCCESS:
        {
            char *s_and_B = (char *)bnet_packet_read(pkt, 64);
            
            bnls_send_LOGONPROOF(bnet, s_and_B);
            
            g_free(s_and_B);
            break;
        }
        case BNET_AUTH_ACCOUNT_DNE:
            //purple_connection_error_reason (gc,
            //    PURPLE_CONNECTION_ERROR_NAME_IN_USE,
            //    "Account does not exist");
            //if (bnet->create_if_dne) {
                // not enabled for w3 until we no longer use BNLS for logon
                //bnet_send_CREATEACCOUNT2(bnet);
            //} else {
                purple_connection_error_reason (gc,
                    PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                    "Account does not exist");
            //}
            break;
        case BNET_AUTH_ACCOUNT_REQUPGRADE:
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                "Account requires upgrade");
            break;
        default:
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                "Account logon failure");
            break;
    }
    
}

static void bnet_recv_AUTH_ACCOUNTLOGONPROOF(BnetConnectionData *bnet, BnetPacket *pkt)
{
    guint32 result = bnet_packet_read_dword(pkt);
    
    PurpleConnection *gc = bnet->account->gc;
    
    switch (result) {
        case BNET_SUCCESS:
            purple_debug_info("bnet", "Logged in!\n");
            if (bnet->create_if_dne) {
                purple_connection_update_progress(gc, "Entering chat", BNET_STEP_FINAL, BNET_STEP_COUNT);
            }
            
            bnet_enter_chat(bnet);
            break;
        case BNET_AUTH_ACCOUNT_BADPW:
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                "Password incorrect");
            break;
        case BNET_AUTH_ACCOUNT_CLOSED:
        {
            char *extra_info = bnet_packet_read_cstring(pkt);
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                "Account closed");
            
            purple_debug_info("bnet", "ACCOUNT CLOSED: %s\n", extra_info);
            g_free(extra_info);
            break;
        }
        case BNET_AUTH_ACCOUNT_REQEMAIL:
            purple_debug_info("bnet", "Logged in!\n");
            if (bnet->create_if_dne) {
                purple_connection_update_progress(gc, "Entering chat", BNET_STEP_FINAL, BNET_STEP_COUNT);
            }
            
            bnet_enter_chat(bnet);
            break;
        case BNET_AUTH_ACCOUNT_ERROR:
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                "Account logon failure");
            break;
    }
}

static void bnet_recv_LOGONRESPONSE2(BnetConnectionData *bnet, BnetPacket *pkt)
{
    guint32 result = bnet_packet_read_dword(pkt);
    
    PurpleConnection *gc = bnet->account->gc;
    
    switch (result) {
        case BNET_SUCCESS:
            purple_debug_info("bnet", "Logged in!\n");
            if (bnet->create_if_dne) {
                purple_connection_update_progress(gc, "Entering chat", BNET_STEP_FINAL, BNET_STEP_COUNT);
            }
            
            bnet_enter_chat(bnet);
            break;
        case BNET_LOGONRESP2_DNE:
            //purple_connection_error_reason (gc,
            //    PURPLE_CONNECTION_ERROR_NAME_IN_USE,
            //    "Account does not exist");
            if (bnet->create_if_dne) {
                bnet_send_CREATEACCOUNT2(bnet);
            } else {
                purple_connection_error_reason (gc,
                    PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                    "Account does not exist");
            }
            break;
        case BNET_LOGONRESP2_BADPW:
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                "Password incorrect");
            break;
        case BNET_LOGONRESP2_CLOSED:
        {
            char *extra_info = bnet_packet_read_cstring(pkt);
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                "Account closed");
            
            purple_debug_info("bnet", "ACCOUNT CLOSED: %s\n", extra_info);
            g_free(extra_info);
            break;
        }
        default:
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                "Account logon failure");
            break;
    }
    
    //purple_debug_info("bnet", "LOGONRESPONSE2: 0x%08x\n", result);
}

static void bnet_recv_CREATEACCOUNT2(BnetConnectionData *bnet, BnetPacket *pkt)
{
    guint32 result = bnet_packet_read_dword(pkt);
    
    PurpleConnection *gc = bnet->account->gc;
    
    switch (result) {
        case BNET_SUCCESS:
            purple_debug_info("bnet", "Account created!\n");
            bnet->create_if_dne = FALSE;
            close(bnet->sbnet.fd);
            break;
        case BNET_CREATEACC2_BADCHAR:
            //purple_connection_error_reason (gc,
            //    PURPLE_CONNECTION_ERROR_NAME_IN_USE,
            //    "Account does not exist");
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                "Account name contains an illigal character");
            break;
        case BNET_CREATEACC2_BADWORD:
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                "Account name contains a banned word");
            break;
        case BNET_CREATEACC2_EXISTS:
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                "Account name in use");
            break;
        case BNET_CREATEACC2_NOTENOUGHALPHA:
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                "Account name does not contain enough alphanumeric characters");
            break;
        default:
            purple_connection_error_reason (gc,
                PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
                "Account create failure");
            break;
    }
    
    //purple_debug_info("bnet", "LOGONRESPONSE2: 0x%08x\n", result);
}

static void bnet_recv_FRIENDSLIST(BnetConnectionData *bnet, BnetPacket *pkt)
{
    guint8 fcount = bnet_packet_read_byte(pkt);
    guint8 idx = 0;
    
    purple_debug_info("bnet", "%d friends on list\n", fcount);
    
    if (fcount > 0) {
        BnetFriendInfo *bfi = g_new0(BnetFriendInfo, fcount);
        while (idx < fcount) {
            bfi[idx].account = bnet_packet_read_cstring(pkt);
            bfi[idx].status = bnet_packet_read_byte(pkt);
            bfi[idx].location = bnet_packet_read_byte(pkt);
            bfi[idx].product = bnet_packet_read_dword(pkt);
            bfi[idx].location_name = bnet_packet_read_cstring(pkt);
            
            bnet_friend_update(bnet, idx, &bfi[idx], FALSE);
            
            idx++;
        }
    }
}

static void bnet_recv_FRIENDSUPDATE(BnetConnectionData *bnet, BnetPacket *pkt)
{
    guint8 index = bnet_packet_read_byte(pkt);
    BnetFriendInfo *bfi = g_list_nth_data(bnet->friends_list, index);
    
    bfi->status = bnet_packet_read_byte(pkt);
    bfi->location = bnet_packet_read_byte(pkt);
    bfi->product = bnet_packet_read_dword(pkt);
    bfi->location_name = bnet_packet_read_cstring(pkt);
    
    bnet_friend_update(bnet, index, bfi, TRUE);
}

static void bnet_recv_FRIENDSADD(BnetConnectionData *bnet, BnetPacket *pkt)
{
    BnetFriendInfo *bfi = g_new0(BnetFriendInfo, 1);
    guint8 index = g_list_length(bnet->friends_list);
    
    bfi->account = bnet_packet_read_cstring(pkt);
    bfi->status = bnet_packet_read_byte(pkt);
    bfi->location = bnet_packet_read_byte(pkt);
    bfi->product = bnet_packet_read_dword(pkt);
    bfi->location_name = bnet_packet_read_cstring(pkt);
    
    bnet_friend_update(bnet, index, bfi, FALSE);
}

static void bnet_recv_FRIENDSREMOVE(BnetConnectionData *bnet, BnetPacket *pkt)
{
    guint8 index = bnet_packet_read_byte(pkt);
    BnetFriendInfo *bfi = g_list_nth_data(bnet->friends_list, index);
    
    purple_buddy_set_protocol_data(bfi->buddy, NULL);
    bnet_friend_info_free(bfi);
    
    bnet->friends_list = g_list_remove(bnet->friends_list,
            g_list_nth(bnet->friends_list, index));
}

static void bnet_recv_FRIENDSPOSITION(BnetConnectionData *bnet, BnetPacket *pkt)
{
    guint8 old_index = bnet_packet_read_byte(pkt);
    guint8 new_index = bnet_packet_read_byte(pkt);
    GList *bfi_link = g_list_nth(bnet->friends_list, old_index);
    
    bnet->friends_list = g_list_remove_link(bnet->friends_list, bfi_link);
    bnet->friends_list = g_list_insert(bnet->friends_list, bfi_link->data, new_index);
}

static void bnet_parse_packet(BnetConnectionData *bnet, guint8 packet_id, guint8 *packet_start, guint16 packet_len)
{
    BnetPacket *pkt = NULL;
    
    purple_debug_misc("bnet", "S>C 0x%02x: length %d\n", packet_id, packet_len);
    
    pkt = bnet_packet_refer(packet_start, packet_len);
    
    switch (packet_id) {
        case BNET_SID_NULL:
            // do nothing bnet_recv_NULL();
            break;
        case BNET_SID_ENTERCHAT:
            bnet_recv_ENTERCHAT(bnet, pkt);
            break;
        case BNET_SID_GETCHANNELLIST:
            bnet_recv_GETCHANNELLIST(bnet, pkt);
            break;
        case BNET_SID_CHATEVENT:
            bnet_recv_CHATEVENT(bnet, pkt);
            break;
        case BNET_SID_MESSAGEBOX:
            bnet_recv_MESSAGEBOX(bnet, pkt);
            break;
        case BNET_SID_PING:
            bnet_recv_PING(bnet, pkt);
            break;
        case BNET_SID_READUSERDATA:
            bnet_recv_READUSERDATA(bnet, pkt);
            break;
        case BNET_SID_AUTH_INFO:
            bnet_recv_AUTH_INFO(bnet, pkt);
            break;
        case BNET_SID_AUTH_CHECK:
            bnet_recv_AUTH_CHECK(bnet, pkt);
            break;
        case BNET_SID_AUTH_ACCOUNTLOGON:
            bnet_recv_AUTH_ACCOUNTLOGON(bnet, pkt);
            break;
        case BNET_SID_AUTH_ACCOUNTLOGONPROOF:
            bnet_recv_AUTH_ACCOUNTLOGONPROOF(bnet, pkt);
            break;
        case BNET_SID_LOGONRESPONSE2:
            bnet_recv_LOGONRESPONSE2(bnet, pkt);
            break;
        case BNET_SID_FRIENDSLIST:
            bnet_recv_FRIENDSLIST(bnet, pkt);
            break;
        case BNET_SID_FRIENDSUPDATE:
            bnet_recv_FRIENDSUPDATE(bnet, pkt);
            break;
        case BNET_SID_FRIENDSADD:
            bnet_recv_FRIENDSADD(bnet, pkt);
            break;
        case BNET_SID_FRIENDSREMOVE:
            bnet_recv_FRIENDSREMOVE(bnet, pkt);
            break;
        case BNET_SID_FRIENDSPOSITION:
            bnet_recv_FRIENDSPOSITION(bnet, pkt);
            break;
        default:
            // unhandled
            purple_debug_warning("bnet", "Received unhandled packet 0x%02x, length %d\n", packet_id, packet_len);
            break;
    }
    
    bnet_packet_free(pkt);
}

/*static void bnet_queue(BnetConnectionData *bnet, BnetQueueElement *qel)
{
    int delay = 1;
    delay += (bnet_packet_get_length(qel->pkt));
    qel->delay = delay;
    
    g_queue_push_head(bnet->action_q, qel);
}

static void bnet_dequeue_tick(BnetConnectionData *bnet)
{
    gboolean last_responded = FALSE;
    if (bnet->active_q_item) {
        if (!bnet->active_q_item->responded) {
            last_responded = TRUE;
        }
    }
    if (!g_queue_is_empty(bnet->action_q)) {
        BnetQueueElement *qel = g_queue_peek_tail(bnet->action_q);
        if (qel->delay > 0 || !last_responded)
            qel->delay -= 100;
        } else {
            bnet_dequeue(bnet);
        }
    }
}


static int bnet_dequeue(BnetConnectionData *bnet)
{
    BnetQueueElement *qel = g_queue_pop_tail(bnet->action_q);
    int ret = -1;
    
    if (bnet->active_q_item) {
        g_free(bnet->active_q_item);
    }
    bnet->active_q_item = qel;
    ret = bnet_packet_send(bnet, qel->pkt, qel->pkt_id, bnet->sbnet.fd);
    if (qel->pkt_response == 0xFF) {
        qel->responded = TRUE;
    }
    
    return ret;
}*/

static gint bnet_channel_user_compare(gconstpointer a, gconstpointer b)
{
    const BnetChannelUser *bcu = a;
    const char *usr = b;
    const char *a_n = NULL;
    const char *b_n = NULL;
    char *a_nc;
    int cmp = 0;
    if (a == NULL || b == NULL) {
        return 1;
    }
    if (bcu->username == NULL) {
        return 1;
    }
    a_n = bnet_normalize(NULL, bcu->username);
    a_nc = g_strdup(a_n);
    b_n = bnet_normalize(NULL, usr);
    cmp = strcmp(a_nc, b_n);
    g_free(a_nc);
    return cmp;
}

static PurpleCmdRet bnet_handle_cmd(PurpleConversation *conv, const gchar *cmdword,
                                  gchar **args, gchar **error, void *data)
{
    struct BnetCommand *c = data;
    PurpleConnection *gc;
    BnetConnectionData *bnet;
    char *cmd;
    char *s_args;

    gc = purple_conversation_get_gc(conv);
    if (!gc)
        return PURPLE_CMD_RET_FAILED;

    bnet = gc->proto_data;
    
    if (!bnet)
        return PURPLE_CMD_RET_FAILED;
    
    if (!c)
        return PURPLE_CMD_RET_FAILED;
        
    if (c->id == 10231) {
        bnet_send_WRITEUSERDATA_2(bnet, args[0], args[1]);
        return PURPLE_CMD_RET_OK;
    }
    
    if (!args) {
        s_args = g_malloc0(1);
    } else {
        s_args = g_strjoinv(" ", args);
        
        if (strlen(s_args) == 0) {
            char *tmp = g_malloc0(1);
            g_free(s_args);
            s_args = tmp;
        } else {
            char *tmp;
            if ((c->bnetflags & BNET_CMD_FLAG_STAROND2) == BNET_CMD_FLAG_STAROND2)
                tmp = g_strdup_printf(" %s%s", bnet->d2_star, s_args);
            else
                tmp = g_strdup_printf(" %s", s_args);
            g_free(s_args);
            s_args = tmp;
        }
    }
    
    if (c->id == BNET_CMD_WHISPER && strlen(s_args) > 1 &&
            g_strstr_len(s_args + 1, strlen(s_args - 1) - 1, " ") != NULL) {
        
        char *who = g_strdup(s_args + 1);
        const char *norm = NULL;
        char *whatloc = g_strstr_len(s_args + 1, strlen(s_args - 1) - 1, " ");
        char *what = g_strdup(whatloc + 1);
        PurpleConvIm *im = NULL;
        PurpleConversation *conv = NULL;
        
        *(who + (whatloc - s_args - 1)) = '\0';
        norm = bnet_d2_normalize(bnet->account, who);
        
        conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, bnet->account, norm);
        im = PURPLE_CONV_IM(conv);
        purple_conversation_present(conv);
        
        purple_conv_im_send(im, what);
        g_free(who);
        g_free(what);
        g_free(s_args);
        return PURPLE_CMD_RET_OK;
    }
    
    cmd = g_strdup_printf("/%s%s", cmdword, s_args);
    if ((c->bnetflags & BNET_CMD_FLAG_INFORESPONSE) == BNET_CMD_FLAG_INFORESPONSE) {
        bnet->last_command_conv = conv;
    } else {
        bnet->last_command_conv = NULL;
    }
    if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM &&
            (c->bnetflags & BNET_CMD_FLAG_WHISPERPRPLCONTINUE) == BNET_CMD_FLAG_WHISPERPRPLCONTINUE) {
        PurpleConvIm *im = purple_conversation_get_im_data(conv);
        if (im) {
            purple_conv_im_send(im, cmd);
        } else {
            bnet_send_CHATCOMMAND(bnet, cmd);
        }
    } else {
        bnet_send_CHATCOMMAND(bnet, cmd);
    }
    
    g_free(cmd);
    g_free(s_args);
    
    if (c->bnetflags & BNET_CMD_FLAG_PRPLCONTINUE) {
        return PURPLE_CMD_RET_CONTINUE;
    }
    
    return PURPLE_CMD_RET_OK;
}

static double get_tz_bias(void)
{
    time_t t_local, t_utc;
    struct tm *tm_utc = NULL;
    
    t_local = time(NULL);
    tm_utc = gmtime(&t_local);
    t_utc = mktime(tm_utc);
    
    return difftime(t_utc, t_local);
}

char *bnet_format_strftime(char *ftime_str)
{
    WINDOWS_FILETIME ft;
    char *space_loc;
    guint64 comb;
    guint32 sec, min, hr, day, mo, yr;
    
    if (strlen(ftime_str) == 0)
        return "(never)";
    
    purple_debug_info("bnet", "ft %s\n", ftime_str);
    ft.dwHighDateTime = (guint32)g_ascii_strtod(ftime_str, &space_loc);
    ft.dwLowDateTime = 0;
    if (space_loc != NULL) {
        ft.dwLowDateTime = (guint32)g_ascii_strtod(space_loc + 1, NULL);
    }
    
    comb = (((guint64)ft.dwHighDateTime) << 32) |
                    ((guint64)ft.dwLowDateTime);
    purple_debug_info("bnet", "ft %d %d\n", ft.dwHighDateTime, ft.dwLowDateTime);
    purple_debug_info("bnet", "ft %I64d\n", comb);
    //purple_debug_info(
    sec = (comb / FT_SECOND) % 60;
    min = (comb / FT_MINUTE) % 60;
    hr =  (comb / FT_HOUR) % 24;
    day = (comb / FT_DAY) + 2;
    mo = MO_JAN;
    yr = 1600;
    while (TRUE) {
        switch (mo) {
            case MO_JAN:
                yr++;
                if (day <= 31) {
                    goto end;
                }
                day -= 31;
                break;
            case MO_FEB:
            {
                int daysinf = 28 + ((yr % 4) ? 0 : 1);
                if (day <= daysinf) {
                    goto end;
                }
                day -= daysinf;
                break;
            }
            case MO_MAR:
            case MO_MAY:
            case MO_JUL:
            case MO_AUG:
            case MO_OCT:
                if (day <= 31) {
                    goto end;
                }
                day -= 31;
                break;
            case MO_APR:
            case MO_JUN:
            case MO_SEP:
            case MO_NOV:
                if (day <= 30) {
                    goto end;
                }
                day -= 30;
                break;
            case MO_DEC:
                if (day <= 31) {
                    goto end;
                }
                day -= 31;
                mo = MO_JAN - 1;
                break;
        }
        mo++;
        continue;
        end: break;
    }
    
    return g_strdup_printf("%04d/%02d/%02d at %02d:%02d:%02d", yr, (mo + 1), (day + 1), hr, min, sec);
}

char *bnet_format_strsec(char *secs_str)
{
    char *days_str;
    
    guint32 secs = g_ascii_strtod(secs_str, NULL);
    guint32 mins = secs / 60;
    guint32 hrs = mins / 60;
    guint32 days = hrs / 24;
    secs %= 60;
    mins %= 60;
    hrs %= 24;
    
    if (strlen(secs_str) == 0 || secs == 0)
        return "now";
    
    if (days == 0) days_str = "";
    else if (days == 1) days_str = " day, ";
    else days_str = " days, ";
    
    return g_strdup_printf("%d%s%02d:%02d:%02d", days, days_str, hrs, mins, secs);
}

static void bnet_friend_update(BnetConnectionData *bnet, int index, BnetFriendInfo *bfi, gboolean replace)
{
    PurpleBuddy *buddy;
    gboolean whoising = FALSE;  
    
    buddy = purple_find_buddy(bnet->account, bfi->account);
    
    if (!buddy) {
        // get or create default "Buddies" group
        PurpleGroup *grp = purple_group_new("Buddies");
        // create a new buddy
        buddy = purple_buddy_new(bnet->account, bfi->account, bfi->account);
        // add to the buddy list
        purple_blist_add_buddy(buddy, NULL, grp, NULL);
    }
    
    purple_buddy_set_protocol_data(buddy, bfi);
    
    bfi->buddy = buddy;
    
    if (replace) {
        bnet->friends_list = g_list_remove(bnet->friends_list,
                g_list_nth(bnet->friends_list, index));
    }
    
    bnet->friends_list = g_list_insert(bnet->friends_list, bfi, index);
    
    if (bfi->location == BNET_FRIEND_LOCATION_OFFLINE) {
        purple_prpl_got_user_status_deactive(bnet->account, bfi->account,
                BNET_STATUS_AWAY);
        purple_prpl_got_user_status_deactive(bnet->account, bfi->account,
                BNET_STATUS_DND);
        purple_prpl_got_user_status(bnet->account, bfi->account,
                BNET_STATUS_OFFLINE, NULL);
    } else {
        purple_prpl_got_user_status(bnet->account, bfi->account,
                BNET_STATUS_ONLINE, NULL);
        
        if (bfi->status & BNET_FRIEND_STATUS_AWAY) {
            purple_prpl_got_user_status(bnet->account, bfi->account,
                    BNET_STATUS_AWAY, NULL);
            
            whoising = TRUE;
        } else {
            purple_prpl_got_user_status_deactive(bnet->account, bfi->account,
                    BNET_STATUS_AWAY);
        }
        
        if (bfi->status & BNET_FRIEND_STATUS_DND) {
            purple_prpl_got_user_status(bnet->account, bfi->account,
                    BNET_STATUS_DND, NULL);
            
            whoising = TRUE;
        } else {
            purple_prpl_got_user_status_deactive(bnet->account, bfi->account,
                    BNET_STATUS_DND);
        }
    }
    
    if (whoising) {
        bfi->automated_lookup = TRUE;
        bnet_whois_user(bnet, bfi->account);
    }
}

static void bnet_close(PurpleConnection *gc)
{
    BnetConnectionData *bnet = gc->proto_data;
    if (bnet != NULL) {
        purple_input_remove(bnet->sbnls.inpa);
        purple_input_remove(bnet->sbnet.inpa);
        bnet->first_join = FALSE;
        bnet->is_online = FALSE;
        
        if (bnet->ka_handle != 0) {
            purple_timeout_remove(bnet->ka_handle);
            bnet->ka_handle = 0;
        }
        
        close(bnet->sbnls.fd);
        if (bnet->sbnls.inbuf != NULL) {
            g_free(bnet->sbnls.inbuf);
            bnet->sbnls.inbuf = NULL;
        }
        if (bnet->bnls_server != NULL) {
            g_free(bnet->bnls_server);
            bnet->bnls_server = NULL;
        }
        close(bnet->sbnet.fd);
        if (bnet->sbnet.inbuf != NULL) {
            g_free(bnet->sbnet.inbuf);
            bnet->sbnet.inbuf = NULL;
        }
        if (bnet->username != NULL) {
            g_free(bnet->username);
            bnet->username = NULL;
        }
        if (bnet->unique_username != NULL) {
            g_free(bnet->unique_username);
            bnet->unique_username = NULL;
        }
        if (bnet->server != NULL) {
            g_free(bnet->server);
            bnet->server = NULL;
        }
        if (bnet->last_sent_to != NULL) {
            g_free(bnet->last_sent_to);
            bnet->last_sent_to = NULL;
        }
        bnet_whois_complete(bnet);
        g_free(bnet);
        bnet = NULL;
    }
}

// bnet doesn't allow anything below ' ' (nulls, tabs, or newlines), so this function is pointless
// but...
// this function ignores the len if > than any NULs, but otherwise sends anything you put into it
static int bnet_send_raw(PurpleConnection *gc, const char *buf, int len)
{
    BnetConnectionData *bnet = gc->proto_data;
    char *mybuf = g_strdup(buf);
    int ret = -1;
    
    if (len < strlen(mybuf)) {
        mybuf[len] = '\0'; // end
    }
    
    ret = bnet_send_CHATCOMMAND(bnet, mybuf);
    
    g_free(mybuf);
    
    return ret;
}

/*If the message is too big to be sent, return -E2BIG.  If
     * the account is not connected, return -ENOTCONN.  If the
     * PRPL is unable to send the message for another reason, return
     * some other negative value.  You can use one of the valid
     * errno values, or just big something.  If the message should
     * not be echoed to the conversation window, return 0.*/
static int bnet_send_whisper(PurpleConnection *gc, const char *who,
                             const char *message, PurpleMessageFlags flags)
{
    BnetConnectionData *bnet = gc->proto_data;
    char *msg_nohtml;
    char *cmd;
    
    if (!bnet->is_online) {
        return -ENOTCONN;
    }
    
    if (strpbrk(message, "\t\v\r\n") != NULL) {
        return -BNET_EBADCHARS;
    }
    
    msg_nohtml = purple_unescape_text(message);
    if (strlen(msg_nohtml) > BNET_MSG_MAXSIZE) {
        return -E2BIG;
    }
    
    cmd = g_strdup_printf("/w %s%s %s",
                bnet->d2_star, who, msg_nohtml);
    bnet_send_CHATCOMMAND(bnet, cmd);
    g_free(cmd);
    
    if (bnet->last_sent_to != NULL) g_free(bnet->last_sent_to);
    bnet->last_sent_to = g_strdup(who);
    bnet->awaiting_whisper_confirm = TRUE;
    
    return strlen(msg_nohtml);
}

    /**
     * Should arrange for purple_notify_userinfo() to be called with
     * @a who's user info.
     */
static void bnet_get_info(PurpleConnection *gc, const char *who)
{
    BnetConnectionData *bnet = gc->proto_data;
    const char *norm = bnet_normalize(bnet->account, who);
    bnet->lookup_user = g_strdup(norm);
    if (!bnet_channeldata_user(bnet, bnet->lookup_user)) {
        bnet_whois_user(bnet, bnet->lookup_user);
    }
    bnet_profiledata_user(bnet, bnet->lookup_user);
}

static void bnet_whois_complete(gpointer user_data)
{
    BnetConnectionData *bnet = (BnetConnectionData *)user_data;
    if (bnet->lookup_user != NULL) {
        g_free(bnet->lookup_user);
        bnet->lookup_user = NULL;
    }
    if (bnet->lookup_info != NULL) {
        purple_notify_user_info_destroy(bnet->lookup_info);
        bnet->lookup_info = NULL;
    }
}

static void bnet_whois_user(BnetConnectionData *bnet, const char *who)
{
    char *cmd = g_strdup_printf("/whois %s%s",
                bnet->d2_star, who);
    bnet_send_CHATCOMMAND(bnet, cmd);
    g_free(cmd);
}

static void bnet_profiledata_user(BnetConnectionData *bnet, const char *who)
{
    gchar *profile_request = BNET_USERDATA_PROFILE_REQUEST;
    gchar *system_request = BNET_USERDATA_SYSTEM_REQUEST;
    gchar *record_request = BNET_USERDATA_RECORD_REQUEST;
    gchar *recordl_request = BNET_USERDATA_RECORD_LADDER_REQUEST;
    gchar *final_request;
    BnetUserDataRequest *req;
    gboolean is_self = FALSE;
    int recordbits = 0;
    char **keys;
    const char *norm = bnet_normalize(bnet->account, who);
    int request_cookie = g_str_hash(norm);
    const char *acct_norm = bnet_account_normalize(bnet->account, norm);
    const char *uu_norm = bnet_normalize(bnet->account, bnet->unique_username);
    
    if (strcmp(uu_norm, acct_norm) == 0) {
        final_request = g_strdup_printf("%s\n%s", profile_request, system_request);
        is_self = TRUE;
    } else {
        final_request = profile_request;
    }
    
    switch (bnet->game) {
        case BNET_GAME_TYPE_SSHR:
            recordbits = BNET_RECORD_NORMAL;
            break;
        case BNET_GAME_TYPE_W2BN:
            recordbits = BNET_RECORD_NORMAL |
                         BNET_RECORD_LADDER |
                         BNET_RECORD_IRONMAN;
            break;
        case BNET_GAME_TYPE_STAR:
        case BNET_GAME_TYPE_SEXP:
        case BNET_GAME_TYPE_JSTR:
            recordbits = BNET_RECORD_NORMAL |
                         BNET_RECORD_LADDER;
            break;
        case BNET_GAME_TYPE_DRTL:
        case BNET_GAME_TYPE_DSHR:
        case BNET_GAME_TYPE_D2DV:
        case BNET_GAME_TYPE_D2XP:
        case BNET_GAME_TYPE_WAR3:
        case BNET_GAME_TYPE_W3XP:
            recordbits = BNET_RECORD_NONE;
            break;
    }
    
    if (recordbits & BNET_RECORD_NORMAL) {
        char *product_id = get_product_id_str(bnet->product_id);
        char *tmp = g_strdup_printf("%s\n%s", final_request,
                    g_strdup_printf(record_request, product_id, 0,
                                                    product_id, 0,
                                                    product_id, 0,
                                                    product_id, 0,
                                                    product_id, 0));
        final_request = tmp;
        g_free(product_id);
    }
    
    if (recordbits & BNET_RECORD_LADDER) {
        char *product_id = get_product_id_str(bnet->product_id);
        char *tmp = g_strdup_printf("%s\n%s", final_request,
                    g_strdup_printf(recordl_request, product_id, 1,
                                                     product_id, 1,
                                                     product_id, 1,
                                                     product_id, 1,
                                                     product_id, 1,
                                                     product_id, 1,
                                                     product_id, 1,
                                                     product_id, 1,
                                                     product_id, 1));
        final_request = tmp;
        g_free(product_id);
    }
    
    if (recordbits & BNET_RECORD_IRONMAN) {
        char *product_id = get_product_id_str(bnet->product_id);
        char *tmp = g_strdup_printf("%s\n%s", final_request,
                    g_strdup_printf(recordl_request, product_id, 3,
                                                     product_id, 3,
                                                     product_id, 3,
                                                     product_id, 3,
                                                     product_id, 3,
                                                     product_id, 3,
                                                     product_id, 3,
                                                     product_id, 3,
                                                     product_id, 3));
        final_request = tmp;
        g_free(product_id);
    }
    
    keys = g_strsplit(final_request, "\n", -1);
    
    req = g_new0(BnetUserDataRequest, 1);
    req->cookie = request_cookie;
    req->request_type = BNET_READUSERDATA_REQUEST_PROFILE |
                        ((is_self) ? BNET_READUSERDATA_REQUEST_SYSTEM : 0) |
                        ((recordbits == BNET_RECORD_NONE) ? 0 : BNET_READUSERDATA_REQUEST_RECORD);
    req->username = g_strdup(acct_norm);
    req->userdata_keys = keys;
    req->product = bnet->product_id;
    bnet->userdata_requests = g_list_append(bnet->userdata_requests, req);
    
    bnet_send_READUSERDATA(bnet, request_cookie, acct_norm, keys);
}

static void bnet_action_set_user_data(PurplePluginAction *action)
{
    PurpleConnection *gc = action->context;
    BnetConnectionData *bnet = gc->proto_data;
    
    if (bnet == NULL) return;
    
    bnet_profile_get_for_edit(bnet);
}

static void bnet_profile_get_for_edit(BnetConnectionData *bnet)
{
    gchar *profile_request = BNET_USERDATA_PROFILE_REQUEST;
    const char *uu_norm = bnet_normalize(bnet->account, bnet->unique_username);
    int request_cookie = g_str_hash(uu_norm);
    BnetUserDataRequest *req;
    char **keys;
    
    keys = g_strsplit(profile_request, "\n", -1);
    
    bnet->writing_profile = TRUE;
    
    req = g_new0(BnetUserDataRequest, 1);
    req->cookie = request_cookie;
    req->request_type = BNET_READUSERDATA_REQUEST_PROFILE;
    req->username = bnet->unique_username;
    req->userdata_keys = keys;
    req->product = bnet->product_id;
    bnet->userdata_requests = g_list_append(bnet->userdata_requests, req);
    
    bnet_send_READUSERDATA(bnet, request_cookie, bnet->unique_username, keys);
}

static void bnet_profile_show_write_dialog(BnetConnectionData *bnet,
        char *psex, char *page, char *ploc, char *pdescr)
{
    PurpleRequestField *field;
    PurpleRequestFields *fields = purple_request_fields_new();
    PurpleRequestFieldGroup *group = purple_request_field_group_new(
            g_strdup_printf("Change profile information for %s", bnet->username));
    
    field = purple_request_field_string_new("profile\\sex", "Sex", psex, FALSE);
    purple_request_field_group_add_field(group, field);
    purple_request_field_string_set_editable(field, TRUE);
    purple_request_field_set_required(field, FALSE);
    purple_request_field_string_set_value(field, psex);
    
    /*field = purple_request_field_string_new("profile\\age", "Age", page, FALSE);
    purple_request_field_string_set_editable(field, FALSE);
    purple_request_field_set_required(field, FALSE);
    purple_request_field_string_set_value(field, page);
    purple_request_field_group_add_field(group, field);*/
    
    field = purple_request_field_string_new("profile\\location", "Location", ploc, FALSE);
    purple_request_field_group_add_field(group, field);
    purple_request_field_string_set_editable(field, TRUE);
    purple_request_field_set_required(field, FALSE);
    purple_request_field_string_set_value(field, ploc);
    
    field = purple_request_field_string_new("profile\\description", "Description", pdescr, TRUE);
    purple_request_field_group_add_field(group, field);
    purple_request_field_string_set_editable(field, TRUE);
    purple_request_field_set_required(field, FALSE);
    purple_request_field_string_set_value(field, pdescr);
    
    purple_request_fields_add_group(fields, group);
    
    bnet->profile_write_fields = fields;
    
    bnet->writing_profile = FALSE;
    
    purple_request_fields(bnet->account->gc, "Edit Profile", NULL, NULL, fields,
            "Save", (GCallback)bnet_profile_write_cb, "Cancel", NULL,
            bnet->account, bnet->username, NULL, bnet);
}

static void bnet_profile_write_cb(gpointer data)
{
    BnetConnectionData *bnet;
    PurpleRequestFields *fields;
    GList *group_list; PurpleRequestFieldGroup *group;
    GList *field_list; PurpleRequestField *field;
    const char *sex, /* *age, */ *location, *description;
    
    bnet = data;
    if (bnet == NULL) return;
    fields = bnet->profile_write_fields;
    if (fields == NULL) return;
    group_list = g_list_first(purple_request_fields_get_groups(fields));
    if (group_list == NULL) return;
    group = group_list->data; // only one group
    if (group == NULL) return;
    field_list = g_list_first(purple_request_field_group_get_fields(group));
    if (field_list == NULL) return;
    
    field = field_list->data;
    sex = purple_request_field_string_get_value(field);
    field_list = g_list_next(field_list);
    
    /*field = field_list->data;
    age = purple_request_field_string_get_value(field);
    field_list = g_list_next(field_list);*/
    
    field = field_list->data;
    location = purple_request_field_string_get_value(field);
    field_list = g_list_next(field_list);
    
    field = field_list->data;
    description = purple_request_field_string_get_value(field);
    
    bnet_send_WRITEUSERDATA(bnet, sex, "", location, description);
    
    //purple_request_fields_destroy(fields);
}

static gboolean bnet_channeldata_user(BnetConnectionData *bnet, const char *who)
{
    GList *li = g_list_find_custom(bnet->channel_users, who, bnet_channel_user_compare);
    BnetChannelUser *bcu;
    char *s_ping;
    char *s_caps = g_malloc0(1);
    BnetProductID product_id;
    char *product;
    //char *s_stats;
    
    char *start; char *loc;
    char *key; char *value;
    guint32 icon_id;
    char *s_clan;
    
    if (li == NULL)
        return FALSE;
    
    bcu = li->data;
    
    s_ping = g_strdup_printf("%dms", bcu->ping);
    
    if (!bnet->lookup_info) {
        bnet->lookup_info = purple_notify_user_info_new();
    } else {
        purple_notify_user_info_add_section_break(bnet->lookup_info);
    }
    
    if (bcu->flags & BNET_USER_FLAG_BLIZZREP) {
        char *tmp = g_strdup("Blizzard Representative");
        g_free(s_caps);
        s_caps = tmp;
    }
    
    if (bcu->flags & BNET_USER_FLAG_OP) {
        if (strlen(s_caps)) {
            char *tmp = g_strdup_printf("%s, Channel Operator", s_caps);
            g_free(s_caps);
            s_caps = tmp;
        } else {
            char *tmp = g_strdup("Channel Operator");
            g_free(s_caps);
            s_caps = tmp;
        }
    }
    
    if (bcu->flags & BNET_USER_FLAG_BNETADMIN) {
        if (strlen(s_caps)) {
            char *tmp = g_strdup_printf("%s, Battle.net Administrator", s_caps);
            g_free(s_caps);
            s_caps = tmp;
        } else {
            char *tmp = g_strdup("Battle.net Administrator");
            g_free(s_caps);
            s_caps = tmp;
        }
    }
    
    if (bcu->flags & BNET_USER_FLAG_NOUDP) {
        if (strlen(s_caps)) {
            char *tmp = g_strdup_printf("%s, No UDP Support", s_caps);
            g_free(s_caps);
            s_caps = tmp;
        } else {
            char *tmp = g_strdup("No UDP Support");
            g_free(s_caps);
            s_caps = tmp;
        }
    }
    
    if (bcu->flags & BNET_USER_FLAG_SQUELCH) {
        if (strlen(s_caps)) {
            char *tmp = g_strdup_printf("%s, Squelched", s_caps);
            g_free(s_caps);
            s_caps = tmp;
        } else {
            char *tmp = g_strdup("Squelched");
            g_free(s_caps);
            s_caps = tmp;
        }
    }
    
    if (strlen(s_caps) == 0) {
        char *tmp = g_strdup("Normal");
        g_free(s_caps);
        s_caps = tmp;
    }
    
    product_id = *((guint32 *)(void *)bcu->stats_data);
    product = get_product_name(product_id);
    
    purple_notify_user_info_add_pair(bnet->lookup_info, "Current location", bnet->channel_name);
    purple_notify_user_info_add_pair(bnet->lookup_info, "Current product", product);
    purple_notify_user_info_add_pair(bnet->lookup_info, "Ping at logon", s_ping);
    purple_notify_user_info_add_pair(bnet->lookup_info, "Channel capabilities", s_caps);
    
    start = g_strdup(bcu->stats_data + 4);
    loc = start;
    switch (product_id) {
        case BNET_PRODUCT_STAR:
        case BNET_PRODUCT_SEXP:
        case BNET_PRODUCT_SSHR:
        case BNET_PRODUCT_JSTR:
        case BNET_PRODUCT_W2BN:
        {
            guint32 l_rating, l_rank, wins, spawn, l_hirating;
            
            loc++;
            l_rating = g_ascii_strtod(loc, &loc);
            loc++;
            l_rank = g_ascii_strtod(loc, &loc);
            loc++;
            wins = g_ascii_strtod(loc, &loc);
            loc++;
            spawn = g_ascii_strtod(loc, &loc);
            loc++;
            g_ascii_strtod(loc, &loc);
            loc++;
            l_hirating = g_ascii_strtod(loc, &loc);
            loc++;
            g_ascii_strtod(loc, &loc);
            loc++;
            g_ascii_strtod(loc, &loc);
            loc++;
            icon_id = *((guint32 *)loc);
            if (l_rating || l_rank || l_hirating) {
                key = g_strdup_printf("%s ladder rating", product);
                value = g_strdup_printf("%d (high: %d)", l_rating, l_hirating);
                purple_notify_user_info_add_pair(bnet->lookup_info, key, value);
                g_free(key); g_free(value);
                
                key = g_strdup_printf("%s ladder rank", product);
                value = g_strdup_printf("%d", l_rank);
                purple_notify_user_info_add_pair(bnet->lookup_info, key, value);
                g_free(key); g_free(value);
            }
            if (wins) {
                key = g_strdup_printf("%s wins", product);
                value = g_strdup_printf("%d", wins);
                purple_notify_user_info_add_pair(bnet->lookup_info, key, value);
                g_free(key); g_free(value);
            }
            if (spawn) {
                purple_notify_user_info_add_pair(bnet->lookup_info, "Spawned client", "Yes");
            }
            break;
        }
        case BNET_PRODUCT_DRTL:
        case BNET_PRODUCT_DSHR:
        {
            char *tmp;
            guint32 char_lvl = 0, char_class = 0, char_dots = 0;
            guint32 char_str = 0, char_mag = 0, char_dex = 0, char_vit = 0;
            guint32 char_gold = 0, spawn = 0;
            
            if (strlen(loc))
                loc++;
            if (strlen(loc))
                char_lvl = g_ascii_strtod(loc, &loc);
            if (strlen(loc))
                loc++;
            if (strlen(loc))
                char_class = g_ascii_strtod(loc, &loc);
            if (strlen(loc))
                loc++;
            if (strlen(loc))
                char_dots = g_ascii_strtod(loc, &loc);
            if (strlen(loc))
                loc++;
            if (strlen(loc))
                char_str = g_ascii_strtod(loc, &loc);
            if (strlen(loc))
                loc++;
            if (strlen(loc))
                char_mag = g_ascii_strtod(loc, &loc);
            if (strlen(loc))
                loc++;
            if (strlen(loc))
                char_dex = g_ascii_strtod(loc, &loc);
            if (strlen(loc))
                loc++;
            if (strlen(loc))
                char_vit = g_ascii_strtod(loc, &loc);
            if (strlen(loc))
                loc++;
            if (strlen(loc))
                char_gold = g_ascii_strtod(loc, &loc);
            if (strlen(loc))
                loc++;
            if (strlen(loc))
                spawn = g_ascii_strtod(loc, NULL);
            
            if (char_lvl) {
                tmp = g_strdup_printf("%d", char_lvl);
                purple_notify_user_info_add_pair(bnet->lookup_info, "Character level", tmp);
                g_free(tmp);
            }
            if (TRUE) { // char_class can = 0
                char *char_type_name;
                switch (char_class) {
                    default: char_type_name = "Unknown";  break;
                    case 0:  char_type_name = "Warrior";  break;
                    case 1:  char_type_name = "Sorcerer"; break;
                    case 2:  char_type_name = "Rogue";    break;
                }
                purple_notify_user_info_add_pair(bnet->lookup_info, "Character class", char_type_name);
            }
            if (TRUE) { // char_dots can = 0
                char *char_diff_text;
                switch (char_dots) {
                    default:
                    case 0: char_diff_text = "None";      break;
                    case 1: char_diff_text = "Normal";    break;
                    case 2: char_diff_text = "Nightmare"; break;
                    case 3: char_diff_text = "Hell";      break;
                }
                purple_notify_user_info_add_pair(bnet->lookup_info, "Last difficulty completed", char_diff_text);
            }
            if (char_str || char_mag || char_dex || char_vit || char_gold) {
                tmp = g_strdup_printf("%d", char_str);
                purple_notify_user_info_add_pair(bnet->lookup_info, "Character strength", tmp);
                g_free(tmp);
                
                tmp = g_strdup_printf("%d", char_mag);
                purple_notify_user_info_add_pair(bnet->lookup_info, "Character magic", tmp);
                g_free(tmp);
                
                tmp = g_strdup_printf("%d", char_dex);
                purple_notify_user_info_add_pair(bnet->lookup_info, "Character dexterity", tmp);
                g_free(tmp);
                
                tmp = g_strdup_printf("%d", char_vit);
                purple_notify_user_info_add_pair(bnet->lookup_info, "Character vitality", tmp);
                g_free(tmp);
                
                tmp = g_strdup_printf("%d", char_gold);
                purple_notify_user_info_add_pair(bnet->lookup_info, "Character gold", tmp);
                g_free(tmp);
            }
            purple_notify_user_info_add_pair(bnet->lookup_info, "Spawned/shareware client", spawn ? "Yes" : "No");
            break;
        }
        case BNET_PRODUCT_D2DV:
        case BNET_PRODUCT_D2XP:
        {
            char *tmp;
            if (strlen(loc) == 0) {
                purple_notify_user_info_add_pair(bnet->lookup_info,
                        "Diablo II character", "an open Battle.net character");
            } else {
                char *realm_name; char *char_name; unsigned char *bytes;
                unsigned char char_type, char_level, char_creation_flags, char_current_act, char_ladder_season;
                char *char_type_name;
                //gboolean is_exp;
                char *char_diff_text;
                
                g_strdelimit(loc, ",", '\0'); // replace commas with nulls for easier reading
                realm_name = loc;
                loc += strlen(realm_name) + 1;
                char_name = loc;
                loc += strlen(char_name) + 1;
                bytes = (unsigned char *)loc;
                char_type = bytes[13];
                char_level = bytes[25];
                char_creation_flags = bytes[26];
                char_current_act = bytes[27];
                char_ladder_season = bytes[30];
                
                switch (char_type) {
                    default:   char_type_name = "Unknown";     break;
                    case 0x01: char_type_name = "Amazon";      break;
                    case 0x02: char_type_name = "Sorceress";   break;
                    case 0x03: char_type_name = "Necromancer"; break;
                    case 0x04: char_type_name = "Paladin";     break;
                    case 0x05: char_type_name = "Barbarian";   break;
                    case 0x06: char_type_name = "Druid";       break;
                    case 0x07: char_type_name = "Assassin";    break;
                }
                
                purple_notify_user_info_add_pair(bnet->lookup_info, "Diablo II realm", realm_name);
                purple_notify_user_info_add_pair(bnet->lookup_info, "Diablo II character", char_name);
                
                tmp = g_strdup_printf("%d", char_level);
                purple_notify_user_info_add_pair(bnet->lookup_info, "Character level", tmp);
                g_free(tmp);
                purple_notify_user_info_add_pair(bnet->lookup_info, "Character class", char_type_name);
                
                //[28] Current act data: 100YYXX0
                // where bits YYXX are distinct for normal
                //  YY=difficulty XX=act because there happens to be 4 acts in normal
                // but in expansion, this is not so
                //  YYXX=act + (difficulty*5)
                // in both cases act goes from 0 to 3 (or 4 on exp, but this isn't used!)
                // and difficulty goes from 0 to 3
                //  when difficulty=3, act=0, means "all acts"
                char_current_act = (char_current_act ^ 0x80) >> 1; // cancel 10000000 bit and ignore lowest bit
                if (char_creation_flags & 0x20) {
                    switch (char_current_act) {
                        default:
                        case 0x0: case 0x1: case 0x2: case 0x3: case 0x4: 
                            char_diff_text = "None";      break;
                        case 0x5: case 0x6: case 0x7: case 0x8: case 0x9: 
                            char_diff_text = "Normal";    break;
                        case 0xA: case 0xB: case 0xC: case 0xD: case 0xE:
                            char_diff_text = "Nightmare"; break;
                        case 0xF:
                            char_diff_text = "Hell";      break;
                    }
                } else {
                    switch (char_current_act >> 2) { // only highest 2 bits matter for norm
                        default:
                        //0000, 0001, 0010, 0011
                        case 0x0:
                            char_diff_text = "None";      break;
                        //0100, 0101, 0110, 0111
                        case 0x1:
                            char_diff_text = "Normal";    break;
                        //1000, 1001, 1010, 1011
                        case 0x2:
                            char_diff_text = "Nightmare"; break;
                        //1100
                        case 0x3:
                            char_diff_text = "Hell";      break;
                    }
                }
                purple_notify_user_info_add_pair(bnet->lookup_info, "Last difficulty completed", char_diff_text);
                
                purple_notify_user_info_add_pair(bnet->lookup_info, "Ladder character", (char_ladder_season == 0xFF) ? "No" : "Yes");
                purple_notify_user_info_add_pair(bnet->lookup_info, "Expansion character", (char_creation_flags & 0x20) ? "Yes" : "No");
                purple_notify_user_info_add_pair(bnet->lookup_info, "Hardcore character", (char_creation_flags & 0x04) ? "Yes" : "No");
                if (char_creation_flags & 0x04) {
                    purple_notify_user_info_add_pair(bnet->lookup_info, "Dead", (char_creation_flags & 0x08) ? "Yes" : "No");
                }
            }
            break;
        }
        case BNET_PRODUCT_WAR3:
        case BNET_PRODUCT_W3XP:
        {
            char *tmp;
            guint32 level = 0;
            int i, clan_len;
            if (strlen(loc)) {
                loc++;
                icon_id = *((guint32 *)loc);
                loc += 5;
                level = g_ascii_strtod(loc, &loc);
                
                if (strlen(loc)) {
                    loc++;
                    clan_len = strlen(loc);
                    s_clan = g_malloc0(5);
                    for (i = 0; i < clan_len && i < 4; i++) {
                        s_clan[i] = loc[clan_len - i - 1];
                    }
                } else {
                    s_clan = g_malloc0(1);
                }
            
                if (level) {
                    tmp = g_strdup_printf("%d", level);
                    purple_notify_user_info_add_pair(bnet->lookup_info, "Warcraft III level", tmp);
                    g_free(tmp);
                }
                if (strlen(s_clan)) {
                    purple_notify_user_info_add_pair(bnet->lookup_info, "Warcraft III clan", s_clan);
                }
                g_free(s_clan);
            }
            break;
        }
    }
    
    g_free(start);
    
    
    purple_notify_userinfo(bnet->account->gc, who,
        bnet->lookup_info, bnet_whois_complete, bnet);
    
    return TRUE;
}

static GHashTable *bnet_chat_info_defaults(PurpleConnection *gc, const char *chat_name)
{
    GHashTable *defaults;

    defaults = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

    if (chat_name != NULL)
        g_hash_table_insert(defaults, "channel", g_strdup(chat_name));

    return defaults;
}

static GList *bnet_chat_info(PurpleConnection *gc)
{
    GList *m = NULL;
    struct proto_chat_entry *pce;

    pce = g_new0(struct proto_chat_entry, 1);
    pce->label = "_Channel:";
    pce->identifier = "channel";
    pce->required = TRUE;
    m = g_list_append(m, pce);

    return m;
}

static char *bnet_channel_message_parse(char *stats_data, BnetChatEventFlags flags, int ping)
{
    BnetProductID product_id = *((guint32 *)(stats_data));
    return g_strdup_printf("%dms using %s", ping, get_product_name(product_id));
}

static PurpleConvChatBuddyFlags bnet_channel_flags_to_prpl_flags(BnetChatEventFlags flags)
{
    PurpleConvChatBuddyFlags result = 0;
    if (flags & BNET_USER_FLAG_BLIZZREP ||
        flags & BNET_USER_FLAG_BNETADMIN) {
        result |= PURPLE_CBFLAGS_FOUNDER;
    }
    if (flags & BNET_USER_FLAG_OP) {
        result |= PURPLE_CBFLAGS_OP;
    }
    if (flags & BNET_USER_FLAG_VOICE) {
        result |= PURPLE_CBFLAGS_VOICE;
    }
    return result;
}

static void bnet_join_chat(PurpleConnection *gc, GHashTable *components)
{
    BnetConnectionData *bnet = gc->proto_data;
    char *room = g_hash_table_lookup(components, "channel");
    char *cmd;
    const char *norm = NULL;
    int chat_id = 0;
    
    if (room == NULL) {
        // the roomlist API stores it in "name" instead of "channel"?
        room = g_hash_table_lookup(components, "name");
        if (room == NULL) {
            // we can't find any room names, get out of here
            return;
        }
    }
    
    norm = bnet_normalize(bnet->account, room);
    chat_id = g_str_hash(norm);
    
    if (bnet->channel_id == chat_id) {
        serv_got_chat_left(gc, bnet->channel_id);
        
        bnet->channel_id = chat_id;
        
        serv_got_joined_chat(gc, chat_id, room);
        
        if (bnet->channel_users != NULL) {
            PurpleConversation *conv = NULL;
            PurpleConvChat *chat = NULL;
            if (!bnet->first_join && bnet->channel_id != 0)
                conv = purple_find_chat(gc, bnet->channel_id);
            if (conv != NULL)
                chat = purple_conversation_get_chat_data(conv);
            if (chat != NULL) {
                GList *li = g_list_first(bnet->channel_users);
                while (li != NULL) {
                    BnetChannelUser *bcu = li->data;
                    purple_conv_chat_add_user(chat, bcu->username,
                             bnet_channel_message_parse(bcu->stats_data, bcu->flags, bcu->ping),
                             bnet_channel_flags_to_prpl_flags(bcu->flags), FALSE);
                    li = g_list_next(li);
                }
            }
        }
        
        return;
    }
    
    bnet->join_attempt = components;
                        
    cmd = g_strdup_printf("/join %s", room);
    bnet_send_CHATCOMMAND(bnet, cmd);
    g_free(cmd);
}

static int bnet_chat_im(PurpleConnection *gc, int chat_id, const char *message, PurpleMessageFlags flags)
{
    BnetConnectionData *bnet = gc->proto_data;
    char *msg_nohtml;
    
    if (!bnet->is_online) {
        return -ENOTCONN;
    }
    if (strpbrk(message, "\t\v\r\n") != NULL) {
        return -BNET_EBADCHARS;
    }
    msg_nohtml = purple_unescape_text(message);
    if (strlen(msg_nohtml) > BNET_MSG_MAXSIZE) {
        return -E2BIG;
    }
    
    if (g_str_has_prefix(msg_nohtml, "/")) {
        PurpleConversation *conv = purple_find_chat(gc, bnet->channel_id);
        PurpleConvChat *chat = NULL;
        if (conv != NULL) {
            chat = purple_conversation_get_chat_data(conv);
        }
        if (chat != NULL) {
            gchar *e = NULL;
            if (purple_cmd_do_command(conv, message + 1,
                    purple_markup_escape_text(message + 1, strlen(message + 1)), &e) ==
                    PURPLE_CMD_STATUS_NOT_FOUND) {
                bnet_send_CHATCOMMAND(bnet, (char *)msg_nohtml);
            }
                
            if (e != NULL) {
                serv_got_chat_in(gc, bnet->channel_id, "", PURPLE_MESSAGE_ERROR, e, time(NULL));
            }
        }
        return 0;
    } else {
        bnet_send_CHATCOMMAND(bnet, (char *)msg_nohtml);
        serv_got_chat_in(gc, bnet->channel_id, bnet->username, PURPLE_MESSAGE_SEND, purple_markup_escape_text(msg_nohtml, strlen(msg_nohtml)), time(NULL));
        return strlen(msg_nohtml);
    }
}

const char *bnet_list_icon(PurpleAccount *a, PurpleBuddy *b)
{
    //star, sexp, d2dv, d2xp, w2bn, war3, w3xp, drtl, chat
    return "bnet";
}

const char *bnet_list_emblem(PurpleBuddy *b)
{
    BnetFriendInfo *bfi = purple_buddy_get_protocol_data(b);
    if (bfi != NULL && bfi->location >= BNET_FRIEND_LOCATION_GAME_PUBLIC) {
        return "game";
    } else if (bfi == NULL) {
        return "not-authorized";
    } else {
        return NULL;
    }
}

char *bnet_status_text(PurpleBuddy *b)
{
    BnetFriendInfo *bfi = purple_buddy_get_protocol_data(b);
    if (bfi == NULL)
        return NULL;
    else if (bfi->stored_status != NULL)
        return bfi->stored_status;
    else
        return NULL;
}

void bnet_tooltip_text(PurpleBuddy *buddy,
                       PurpleNotifyUserInfo *info,
                       gboolean full)
{
    BnetFriendInfo *bfi = purple_buddy_get_protocol_data(buddy);
    purple_debug_info("bnet", "poll buddy tooltip %s \n", buddy->name);
    if (bfi == NULL) {
        // no information saved
        if (full) {
            purple_notify_user_info_add_pair(info, "Status", "Not on Battle.net's friend list.");
        }
    } else if (bfi->location != BNET_FRIEND_LOCATION_OFFLINE) {
        // add things to online friends
        purple_notify_user_info_add_pair(info, "Has you",
                (bfi->status & BNET_FRIEND_STATUS_MUTUAL) ? "Yes" : "No");
        
        if (full) {
            purple_notify_user_info_add_pair(info, "Location",
                    get_location_text(bfi->location, bfi->location_name));
            purple_notify_user_info_add_pair(info, "Product",
                    get_product_name(bfi->product));
        }
        
        if (bfi->status & BNET_FRIEND_STATUS_DND) {
            purple_notify_user_info_add_pair(info, "Status",
                    g_strdup_printf("Do Not Disturb - %s", bfi->stored_status));
        } else if (bfi->status & BNET_FRIEND_STATUS_AWAY) {
            purple_notify_user_info_add_pair(info, "Status",
                    g_strdup_printf("Away - %s", bfi->stored_status));
        } else {
            purple_notify_user_info_add_pair(info, "Status", "Available");
        }
    }
}

char *get_location_text(BnetFriendLocation location, char *location_name)
{
    switch (location)
    {
        case BNET_FRIEND_LOCATION_OFFLINE:
            return "Offline";
        default:
        case BNET_FRIEND_LOCATION_ONLINE:
            return "Nowhere";
        case BNET_FRIEND_LOCATION_CHANNEL:
            if (strlen(location_name) > 0) {
                return g_strdup_printf("In channel %s", location_name);
            } else {
                return "In a private channel";
            }
        case BNET_FRIEND_LOCATION_GAME_PUBLIC:
            if (strlen(location_name) > 0) {
                return g_strdup_printf("In the public game %s", location_name);
            } else {
                return "In a public game";
            }
        case BNET_FRIEND_LOCATION_GAME_PRIVATE:
            if (strlen(location_name) > 0) {
                return g_strdup_printf("In the private game %s", location_name);
            } else {
                return "In a private game";
            }
        case BNET_FRIEND_LOCATION_GAME_PROTECTED:
            if (strlen(location_name) > 0) {
                return g_strdup_printf("In the password protected game %s", location_name);
            } else {
                return "In a password protected game";
            }
    }
}

char *get_product_name(BnetProductID product)
{
    switch (product)
    {
        case BNET_PRODUCT_STAR:
        case BNET_GAME_TYPE_STAR:
            return "Starcraft";
        case BNET_PRODUCT_SEXP:
        case BNET_GAME_TYPE_SEXP:
            return "Starcraft Broodwar";
        case BNET_PRODUCT_W2BN:
        case BNET_GAME_TYPE_W2BN:
            return "Warcraft II";
        case BNET_PRODUCT_D2DV:
        case BNET_GAME_TYPE_D2DV:
            return "Diablo II";
        case BNET_PRODUCT_D2XP:
        case BNET_GAME_TYPE_D2XP:
            return "Diablo II Lord of Destruction";
        case BNET_PRODUCT_WAR3:
        case BNET_GAME_TYPE_WAR3:
            return "Warcraft III";
        case BNET_PRODUCT_W3XP:
        case BNET_GAME_TYPE_W3XP:
            return "Warcraft III The Frozen Throne";
        case BNET_PRODUCT_DRTL:
        case BNET_GAME_TYPE_DRTL:
            return "Diablo";
        case BNET_PRODUCT_DSHR:
        case BNET_GAME_TYPE_DSHR:
            return "Diablo Shareware";
        case BNET_PRODUCT_SSHR:
        case BNET_GAME_TYPE_SSHR:
            return "Starcraft Shareware";
        case BNET_PRODUCT_JSTR:
        case BNET_GAME_TYPE_JSTR:
            return "Starcraft Japanese";
        case BNET_PRODUCT_CHAT:
            return "Telnet Chat";
        default:
            return "Unknown";
    }
}

char *get_product_id_str(BnetProductID product)
{
    guint32 *pproduct;
    char *ret;
    
    switch (product)
    {
        case BNET_GAME_TYPE_STAR: product = BNET_PRODUCT_STAR; break;
        case BNET_GAME_TYPE_SEXP: product = BNET_PRODUCT_SEXP; break;
        case BNET_GAME_TYPE_W2BN: product = BNET_PRODUCT_W2BN; break;
        case BNET_GAME_TYPE_D2DV: product = BNET_PRODUCT_D2DV; break;
        case BNET_GAME_TYPE_D2XP: product = BNET_PRODUCT_D2XP; break;
        case BNET_GAME_TYPE_WAR3: product = BNET_PRODUCT_WAR3; break;
        case BNET_GAME_TYPE_W3XP: product = BNET_PRODUCT_W3XP; break;
        case BNET_GAME_TYPE_DRTL: product = BNET_PRODUCT_DRTL; break;
        case BNET_GAME_TYPE_DSHR: product = BNET_PRODUCT_DSHR; break;
        case BNET_GAME_TYPE_SSHR: product = BNET_PRODUCT_SSHR; break;
        case BNET_GAME_TYPE_JSTR: product = BNET_PRODUCT_JSTR; break;
    }
    
    pproduct = &product;
    ret = g_malloc0(5);
    ret[0] = *(((char *)pproduct) + 3);
    ret[1] = *(((char *)pproduct) + 2);
    ret[2] = *(((char *)pproduct) + 1);
    ret[3] = *(((char *)pproduct) + 0);
    ret[4] = 0;
    return ret;
}

static GList *bnet_status_types(PurpleAccount *account)
{
    PurpleStatusType *type;
    GList *types = NULL;

    type = purple_status_type_new(PURPLE_STATUS_AVAILABLE,
        BNET_STATUS_ONLINE, NULL, TRUE);
    types = g_list_append(types, type);

    type = purple_status_type_new_with_attrs(PURPLE_STATUS_AWAY,
        BNET_STATUS_AWAY, NULL, TRUE, TRUE, FALSE,
        "message", "Message", purple_value_new(PURPLE_TYPE_STRING),
        NULL);
    types = g_list_append(types, type);

    type = purple_status_type_new_with_attrs(PURPLE_STATUS_UNAVAILABLE, 
        BNET_STATUS_DND, NULL, TRUE, TRUE, FALSE,
        "message", "Message", purple_value_new(PURPLE_TYPE_STRING),
        NULL);
    types = g_list_append(types, type);

    type = purple_status_type_new(PURPLE_STATUS_OFFLINE,
        BNET_STATUS_OFFLINE, NULL, TRUE);
    types = g_list_append(types, type);

    return types;
}

static void bnet_add_buddy(PurpleConnection *gc, PurpleBuddy *buddy, PurpleGroup *group)
{
    BnetConnectionData *bnet = gc->proto_data;
    
    const char *username = purple_buddy_get_name(buddy);

    char *cmd = g_strdup_printf("/f a %s",
                username);
    bnet_send_CHATCOMMAND(bnet, cmd);
    g_free(cmd);
}

static void bnet_remove_buddy(PurpleConnection *gc, PurpleBuddy *buddy, PurpleGroup *group)
{
    BnetConnectionData *bnet = gc->proto_data;
    
    const char *username = purple_buddy_get_name(buddy);
    BnetFriendInfo *bfi = purple_buddy_get_protocol_data(buddy);
    
    char *cmd;
    
    // the buddy wasn't on our friends list
    if (bfi == NULL) return;
    
    cmd = g_strdup_printf("/f r %s", username);
    bnet_send_CHATCOMMAND(bnet, cmd);
    g_free(cmd);
}

static PurpleRoomlist *bnet_roomlist_get_list(PurpleConnection *gc)
{
    BnetConnectionData *bnet = gc->proto_data;
    GList *fields = NULL;
    PurpleRoomlistField *f;
    PurpleRoomlistRoom *r;

    if (bnet->room_list)
        purple_roomlist_unref(bnet->room_list);

    bnet->room_list = purple_roomlist_new(purple_connection_get_account(gc));

    f = purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, "", "channel", TRUE);
    fields = g_list_append(fields, f);
    
    purple_roomlist_set_fields(bnet->room_list, fields);

    if (bnet->channel_list != NULL) {
        GList *room_el = g_list_first(bnet->channel_list);
        r = purple_roomlist_room_new(PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            (char *)room_el->data, NULL);
        purple_roomlist_room_add(bnet->room_list, r);
        while (g_list_next(room_el) != NULL) {
            room_el = g_list_next(room_el);
            r = purple_roomlist_room_new(PURPLE_ROOMLIST_ROOMTYPE_ROOM,
                (char *)room_el->data, NULL);
            purple_roomlist_room_add(bnet->room_list, r);
        }
    }
    
    purple_roomlist_set_in_progress(bnet->room_list, FALSE);

    return bnet->room_list;
}

static void bnet_roomlist_cancel(PurpleRoomlist *list)
{
    PurpleConnection *gc = purple_account_get_connection(list->account);
    BnetConnectionData *bnet;

    if (gc == NULL)
        return;

    bnet = gc->proto_data;

    purple_roomlist_set_in_progress(list, FALSE);

    if (bnet->room_list == list) {
        bnet->room_list = NULL;
        purple_roomlist_unref(list);
    }
}

static void bnet_set_status(PurpleAccount *account, PurpleStatus *status)
{   
    const char *msg = purple_status_get_attr_string(status, "message");
    const char *type = purple_status_get_name(status);
    PurpleConnection *gc = purple_account_get_connection(account);
    BnetConnectionData *bnet = gc->proto_data;
    
    if (purple_status_is_online(status)) {
        if (purple_status_is_available(status)) {
            // unset away and dnd
            if (bnet->is_away) {
                bnet_set_away(bnet, FALSE, NULL);
            }
            if (bnet->is_dnd) {
                bnet_set_dnd(bnet, FALSE, NULL);
            }
        } else {
            if (strcmp(type, BNET_STATUS_AWAY) == 0) {
                if (bnet->is_dnd) {
                    bnet_set_dnd(bnet, FALSE, NULL);
                }
                bnet_set_away(bnet, TRUE, msg);
            } else if (strcmp(type, BNET_STATUS_DND) == 0) {
                if (bnet->is_away) {
                    bnet_set_away(bnet, FALSE, NULL);
                }
                bnet_set_dnd(bnet, TRUE, msg);
            }
        }
    }
}

void bnet_set_away(BnetConnectionData *bnet, gboolean new_state, const gchar *message)
{
    char *msg;
    if (message == NULL || strlen(message) == 0)
        msg = g_strdup("Not available");
    else
        msg = g_strdup(message);
    
    bnet->setting_away_status = TRUE;
    if (new_state) {
        char *cmd = g_strdup_printf("/away %s", msg);
        bnet_send_CHATCOMMAND(bnet, cmd);
        g_free(cmd);
        
        bnet->away_msg = msg;
    } else {
        char *cmd = "/away";
        bnet_send_CHATCOMMAND(bnet, cmd);
        
        bnet->away_msg = NULL;
        g_free(msg);
    }
}

void bnet_set_dnd(BnetConnectionData *bnet, gboolean new_state, const gchar *message)
{
    char *msg;
    if (message == NULL || strlen(message) == 0)
        msg = g_strdup("Not available");
    else
        msg = g_strdup(message);
    
    bnet->setting_dnd_status = TRUE;
    if (new_state) {
        char *cmd = g_strdup_printf("/dnd %s", msg);
        bnet_send_CHATCOMMAND(bnet, cmd);
        g_free(cmd);
        
        bnet->dnd_msg = msg;
    } else {
        char *cmd = "/dnd";
        bnet_send_CHATCOMMAND(bnet, cmd);
        
        bnet->dnd_msg = NULL;
        g_free(msg);
    }
}

static const char *bnet_normalize(const PurpleAccount *account, const char *in)
{
    static char out[64];
    
    char *o = g_ascii_strdown(in, strlen(in));
    g_memmove((char *)out, o, strlen(o) + 1);
    g_free(o);
    
    return out;
}

static const char *bnet_d2_normalize(const PurpleAccount *account, const char *in)
{
    PurpleConnection *gc = NULL;
    BnetConnectionData *bnet = NULL;
    static char o[64];
    char *d2norm = NULL;
    
    if (account != NULL) gc = purple_account_get_connection(account);
    
    if (gc != NULL) bnet = gc->proto_data;
    
    if (bnet != NULL && bnet_is_d2(bnet))
    {
        char *d2_star = g_strstr_len(in, 30, "*");
        if (d2_star != NULL) {
            // CHARACTER*NAME
            // CHARACTER (*NAME)
            // or *NAME
            d2norm = g_strdup(d2_star + 1);
            if (d2_star > in + 1) {
                if (*(d2_star - 1) == '(') {
                    // CHARACTER (*NAME)
                    // remove last character ")"
                    *(d2norm + strlen(d2norm) - 1) = '\0';
                }
            }
        }
    }
    if (d2norm == NULL) {
        d2norm = g_strdup(in);
    }
    
    g_memmove((char *)o, d2norm, strlen(d2norm) + 1);
    g_free(d2norm);
    return o;
}

// removes account numbers from accounts (Ribose#2 > Ribose; Ribose#2@Azeroth > Ribose@Azeroth)
// for SID_READUSERDATA.
static const char *bnet_account_normalize(const PurpleAccount *account, const char *in)
{
    static char o[64];
    char *out = g_strdup(in);
    char *poundloc = NULL;
    poundloc = g_strstr_len(out, strlen(out), "#");
    if (poundloc != NULL) {
        int i = 0, j = 0;
        gboolean is_gateway = FALSE;
        for (; i < strlen(poundloc); ) {
            if (poundloc[i] == '@')
                is_gateway = TRUE;
            
            // Ribose\02\0
            // Ribose@Azeroth\0h\0
            
            if (is_gateway) {
                poundloc[j] = poundloc[i];
                j++;
            }
            
            i++;
        }
        poundloc[j] = poundloc[i];
    }
    g_memmove((char *)o, out, strlen(out) + 1);
    g_free(out);
    return o;
}

static gboolean bnet_is_d2(BnetConnectionData *bnet)
{
    return (bnet->product_id == BNET_PRODUCT_D2DV ||
            bnet->product_id == BNET_PRODUCT_D2XP);
}

static gboolean bnet_is_w3(BnetConnectionData *bnet)
{
    return (bnet->product_id == BNET_PRODUCT_WAR3 ||
            bnet->product_id == BNET_PRODUCT_W3XP);
}

static GList *bnet_actions(PurplePlugin *plugin, gpointer context)
{
    GList *list = NULL;
    PurplePluginAction *action = NULL;
    
    action = purple_plugin_action_new("Set User Info...", bnet_action_set_user_data);
    
    list = g_list_append(list, action);
    
    return list;
}

static PurplePluginProtocolInfo prpl_info =
{
    OPT_PROTO_CHAT_TOPIC | OPT_PROTO_SLASH_COMMANDS_NATIVE,
    NULL,                   /* user_splits */
    NULL,                   /* protocol_options */
    NO_BUDDY_ICONS,         /* icon_spec */
    bnet_list_icon,         /* list_icon */
    bnet_list_emblem,       /* list_emblems */
    bnet_status_text,       /* status_text */
    bnet_tooltip_text,      /* tooltip_text */
    bnet_status_types,      /* away_states */
    NULL,                   /* blist_node_menu */
    bnet_chat_info,         /* chat_info */
    bnet_chat_info_defaults,/* chat_info_defaults */
    bnet_login,             /* login */
    bnet_close,             /* close */
    bnet_send_whisper,      /* send_im */
    NULL,                   /* set_info */
    NULL,                   /* send_typing */
    bnet_get_info,          /* get_info */
    bnet_set_status,        /* set_status */
    NULL,                   /* set_idle */
    bnet_account_chpw,      /* change_passwd */
    bnet_add_buddy,         /* add_buddy */
    NULL,                   /* add_buddies */
    bnet_remove_buddy,      /* remove_buddy */
    NULL,                   /* remove_buddies */
    NULL,                   /* add_permit */
    NULL,                   /* add_deny */
    NULL,                   /* rem_permit */
    NULL,                   /* rem_deny */
    NULL,                   /* set_permit_deny */
    bnet_join_chat,         /* join_chat */
    NULL,                   /* reject_chat */
    NULL,                   /* get_chat_name */
    NULL,                   /* chat_invite */
    NULL,                   /* chat_leave */
    NULL,                   /* chat_whisper */
    bnet_chat_im,           /* chat_send */
    NULL,                   /* keepalive */
    bnet_account_register,  /* register_user */
    NULL,                   /* get_cb_info */
    NULL,                   /* get_cb_away */
    NULL,                   /* alias_buddy */
    NULL,                   /* group_buddy */
    NULL,                   /* rename_group */
    bnet_buddy_free,        /* buddy_free */
    NULL,                   /* convo_closed */
    bnet_normalize,         /* normalize */
    NULL,                   /* set_buddy_icon */
    NULL,                   /* remove_group */
    NULL,                   /* get_cb_real_name */
    NULL,                   /* set_chat_topic */
    NULL,                   /* find_blist_chat */
    bnet_roomlist_get_list, /* roomlist_get_list */
    bnet_roomlist_cancel,   /* roomlist_cancel */
    NULL,                   /* roomlist_expand_category */
    NULL,                   /* can_receive_file */
    NULL,                   /* send_file */
    NULL,                   /* new_xfer */
    NULL,                   /* offline_message */
    NULL,                   /* whiteboard_prpl_ops */
    bnet_send_raw,          /* send_raw */
    NULL,                   /* roomlist_room_serialize */
    NULL,                   /* unregister_user */
    NULL,                   /* send_attention */
    NULL,                   /* get_attention_types */
    sizeof(PurplePluginProtocolInfo),  
                            /* struct_size */
    NULL,                   /* get_account_text_table */
    NULL,                   /* initiate_media */
    NULL,                   /* get_media_caps */
    NULL,                   /* get_moods */
    NULL,                   /* set_public_alias */
    NULL                    /* get_public_alias */
};

static PurplePluginInfo info =
{
    PURPLE_PLUGIN_MAGIC,
    PURPLE_MAJOR_VERSION,
    PURPLE_MINOR_VERSION,
    PURPLE_PLUGIN_PROTOCOL,                           /**< type           */
    NULL,                                             /**< ui_requirement */
    0,                                                /**< flags          */
    NULL,                                             /**< dependencies   */
    PURPLE_PRIORITY_DEFAULT,                          /**< priority       */

    PLUGIN_ID,                                        /**< id             */
    PLUGIN_NAME,                                      /**< name           */
    PLUGIN_STR_VER,                                   /**< version        */
    PLUGIN_SHORT_DESCR,                               /**  summary        */
    PLUGIN_DESCR,                                     /**  description    */
    PLUGIN_AUTHOR,                                    /**< author         */
    PLUGIN_WEBSITE,                                   /**< homepage       */

    NULL,                                             /**< load           */
    NULL,                                             /**< unload         */
    NULL,                                             /**< destroy        */

    NULL,                                             /**< ui_info        */
    &prpl_info,                                       /**< extra_info     */
    NULL,                                             /**< prefs_info     */
    bnet_actions,

    /* padding */
    NULL,
    NULL,
    NULL,
    NULL
};

static void                        
init_plugin(PurplePlugin *plugin)
{               
    PurpleAccountUserSplit *split = NULL;
    PurpleAccountOption *option = NULL;
    GList *optlist = NULL;
    PurpleKeyValuePair *kvp = NULL;
    char *prpl_name = PLUGIN_ID;
    PurpleCmdFlag flags = PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_IM |
                                PURPLE_CMD_FLAG_PRPL_ONLY | PURPLE_CMD_FLAG_ALLOW_WRONG_ARGS;
    struct BnetCommand *c = NULL;

    split = purple_account_user_split_new("Server", BNET_DEFAULT_SERVER, '@');
    prpl_info.user_splits = g_list_append(prpl_info.user_splits, split);
    
    option = purple_account_option_int_new("Port", "port", BNET_DEFAULT_PORT);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);
    
    kvp = g_new0(PurpleKeyValuePair, 1);
    kvp->key = "StarCraft";
    kvp->value = "RATS";
    optlist = g_list_append(optlist, kvp);
    
    kvp = g_new0(PurpleKeyValuePair, 1);
    kvp->key = "StarCraft: Brood War";
    kvp->value = "PXES";
    optlist = g_list_append(optlist, kvp);
    
    kvp = g_new0(PurpleKeyValuePair, 1);
    kvp->key = "WarCraft II: Battle.net Edition";
    kvp->value = "NB2W";
    optlist = g_list_append(optlist, kvp);
    
    kvp = g_new0(PurpleKeyValuePair, 1);
    kvp->key = "Diablo II";
    kvp->value = "VD2D";
    optlist = g_list_append(optlist, kvp);
    
    kvp = g_new0(PurpleKeyValuePair, 1);
    kvp->key = "Diablo II: Lord of Destruction";
    kvp->value = "PX2D";
    optlist = g_list_append(optlist, kvp);
    
    kvp = g_new0(PurpleKeyValuePair, 1);
    kvp->key = "WarCraft III";
    kvp->value = "3RAW";
    optlist = g_list_append(optlist, kvp);
    
    kvp = g_new0(PurpleKeyValuePair, 1);
    kvp->key = "WarCraft III: The Frozen Throne";
    kvp->value = "PX3W";
    optlist = g_list_append(optlist, kvp);
    
    //kvp = g_new0(PurpleKeyValuePair, 1);
    //kvp->key = "StarCraft: Japanese";
    //kvp->value = "RTSJ";
    //optlist = g_list_append(optlist, kvp);
    //
    //kvp = g_new0(PurpleKeyValuePair, 1);
    //kvp->key = "StarCraft: Shareware";
    //kvp->value = "RHSS";
    //optlist = g_list_append(optlist, kvp);
    //
    //kvp = g_new0(PurpleKeyValuePair, 1);
    //kvp->key = "Diablo";
    //kvp->value = "LTRD";
    //optlist = g_list_append(optlist, kvp);
    //
    //kvp = g_new0(PurpleKeyValuePair, 1);
    //kvp->key = "Diablo: Shareware";
    //kvp->value = "RHSD";
    //optlist = g_list_append(optlist, kvp);
    
    option = purple_account_option_list_new("Game Client", "product", optlist);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);
    
    option = purple_account_option_string_new("CD Key", "key1", "");
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);
    
    option = purple_account_option_string_new("Expansion CD Key", "key2", "");
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);
    
    option = purple_account_option_string_new("Key Owner", "key_owner", "");
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);
    
    option = purple_account_option_string_new("Logon Server", "bnlsserver", BNET_DEFAULT_BNLSSERVER);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);
    
    option = purple_account_option_bool_new("Hide mutual friend status-change messages", "hidemutual", TRUE);
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);
    
    for (c = bnet_cmds; c && c->name; c++) {
        purple_cmd_register(c->name, c->args, PURPLE_CMD_P_PRPL, flags,
                prpl_name, bnet_handle_cmd, c->helptext, c);
    }
}

PURPLE_INIT_PLUGIN(clbnet, init_plugin, info)

#endif
