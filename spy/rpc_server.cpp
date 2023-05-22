﻿#pragma warning(disable : 4251)

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include <nng/nng.h>
#include <nng/protocol/pair1/pair.h>
#include <nng/supplemental/util/platform.h>

#include "wcf.pb.h"

#include "accept_new_friend.h"
#include "add_chatroom_member.h"
#include "decrypt_image.h"
#include "exec_sql.h"
#include "get_contacts.h"
#include "log.h"
#include "pb_types.h"
#include "pb_util.h"
#include "receive_msg.h"
#include "receive_transfer.h"
#include "rpc_server.h"
#include "send_msg.h"
#include "spy.h"
#include "spy_types.h"
#include "user_info.h"
#include "util.h"

#define URL_SIZE   20
#define BASE_URL   "tcp://0.0.0.0"
#define G_BUF_SIZE (16 * 1024 * 1024)

extern int IsLogin(void); // Defined in spy.cpp

bool gIsListening;
mutex gMutex;
condition_variable gCV;
queue<WxMsg_t> gMsgQueue;

static int lport       = 0;
static DWORD lThreadId = 0;
static bool lIsRunning = false;
static nng_socket sock;
static uint8_t gBuffer[G_BUF_SIZE] = { 0 };

bool func_is_login(uint8_t *out, size_t *len)
{
    Response rsp   = Response_init_default;
    rsp.func       = Functions_FUNC_IS_LOGIN;
    rsp.which_msg  = Response_status_tag;
    rsp.msg.status = IsLogin();

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_get_self_wxid(uint8_t *out, size_t *len)
{
    string wxid   = GetSelfWxid();
    Response rsp  = Response_init_default;
    rsp.func      = Functions_FUNC_GET_SELF_WXID;
    rsp.which_msg = Response_str_tag;
    rsp.msg.str   = (char *)wxid.c_str();

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_get_msg_types(uint8_t *out, size_t *len)
{
    Response rsp  = Response_init_default;
    rsp.func      = Functions_FUNC_GET_MSG_TYPES;
    rsp.which_msg = Response_types_tag;

    MsgTypes_t types                 = GetMsgTypes();
    rsp.msg.types.types.funcs.encode = encode_types;
    rsp.msg.types.types.arg          = &types;

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_get_contacts(uint8_t *out, size_t *len)
{
    Response rsp  = Response_init_default;
    rsp.func      = Functions_FUNC_GET_CONTACTS;
    rsp.which_msg = Response_contacts_tag;

    vector<RpcContact_t> contacts          = GetContacts();
    rsp.msg.contacts.contacts.funcs.encode = encode_contacts;
    rsp.msg.contacts.contacts.arg          = &contacts;

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_get_db_names(uint8_t *out, size_t *len)
{
    Response rsp  = Response_init_default;
    rsp.func      = Functions_FUNC_GET_DB_NAMES;
    rsp.which_msg = Response_dbs_tag;

    DbNames_t dbnames              = GetDbNames();
    rsp.msg.dbs.names.funcs.encode = encode_dbnames;
    rsp.msg.dbs.names.arg          = &dbnames;

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_get_db_tables(char *db, uint8_t *out, size_t *len)
{
    Response rsp  = Response_init_default;
    rsp.func      = Functions_FUNC_GET_DB_TABLES;
    rsp.which_msg = Response_tables_tag;

    DbTables_t tables                  = GetDbTables(db);
    rsp.msg.tables.tables.funcs.encode = encode_tables;
    rsp.msg.tables.tables.arg          = &tables;

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_get_user_info(uint8_t *out, size_t *len)
{
    Response rsp  = Response_init_default;
    rsp.func      = Functions_FUNC_GET_USER_INFO;
    rsp.which_msg = Response_ui_tag;

    UserInfo_t ui     = GetUserInfo();
    rsp.msg.ui.wxid   = (char *)ui.wxid.c_str();
    rsp.msg.ui.name   = (char *)ui.name.c_str();
    rsp.msg.ui.mobile = (char *)ui.mobile.c_str();
    rsp.msg.ui.home   = (char *)ui.home.c_str();

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_send_txt(TextMsg txt, uint8_t *out, size_t *len)
{
    Response rsp   = Response_init_default;
    rsp.func       = Functions_FUNC_SEND_TXT;
    rsp.which_msg  = Response_status_tag;
    rsp.msg.status = 0;

    if ((txt.msg == NULL) || (txt.receiver == NULL)) {
        rsp.msg.status = -1; // Empty message or empty receiver
    } else {
        string msg(txt.msg);
        string receiver(txt.receiver);
        string aters(txt.aters ? txt.aters : "");

        SendTextMessage(receiver, msg, aters);
    }

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_send_img(char *path, char *receiver, uint8_t *out, size_t *len)
{
    Response rsp   = Response_init_default;
    rsp.func       = Functions_FUNC_SEND_IMG;
    rsp.which_msg  = Response_status_tag;
    rsp.msg.status = 0;

    if ((path == NULL) || (receiver == NULL)) {
        rsp.msg.status = -1;
    } else {
        SendImageMessage(receiver, path);
    }

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_send_file(char *path, char *receiver, uint8_t *out, size_t *len)
{
    Response rsp   = Response_init_default;
    rsp.func       = Functions_FUNC_SEND_IMG;
    rsp.which_msg  = Response_status_tag;
    rsp.msg.status = 0;

    if ((path == NULL) || (receiver == NULL)) {
        rsp.msg.status = -1;
    } else {
        SendImageMessage(receiver, path);
    }

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_send_xml(XmlMsg xml, uint8_t *out, size_t *len)
{
    Response rsp   = Response_init_default;
    rsp.func       = Functions_FUNC_SEND_XML;
    rsp.which_msg  = Response_status_tag;
    rsp.msg.status = 0;

    if ((xml.content == NULL) || (xml.receiver == NULL)) {
        rsp.msg.status = -1;
    } else {
        string receiver(xml.receiver);
        string content(xml.content);
        string path(xml.path ? xml.path : "");
        uint32_t type = (uint32_t)xml.type;
        SendXmlMessage(receiver, content, path, type);
    }

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_send_emotion(char *path, char *receiver, uint8_t *out, size_t *len)
{
    Response rsp   = Response_init_default;
    rsp.func       = Functions_FUNC_SEND_EMOTION;
    rsp.which_msg  = Response_status_tag;
    rsp.msg.status = 0;

    if ((path == NULL) || (receiver == NULL)) {
        rsp.msg.status = -1;
    } else {
        SendEmotionMessage(receiver, path);
    }

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

static void PushMessage()
{
    static nng_socket msg_sock;
    static uint8_t buffer[G_BUF_SIZE] = { 0 };

    int rv;
    Response rsp  = Response_init_default;
    rsp.func      = Functions_FUNC_ENABLE_RECV_TXT;
    rsp.which_msg = Response_wxmsg_tag;

    pb_ostream_t stream = pb_ostream_from_buffer(buffer, G_BUF_SIZE);

    char url[URL_SIZE + 1] = { 0 };
    sprintf_s(url, URL_SIZE, "%s:%d", BASE_URL, lport + 1);
    LOG_ERROR("URL: {}", url);
    if ((rv = nng_pair1_open(&msg_sock)) != 0) {
        LOG_ERROR("nng_pair0_open error {}", nng_strerror(rv));
        return;
    }

    if ((rv = nng_listen(msg_sock, url, NULL, 0)) != 0) {
        LOG_ERROR("nng_listen error {}", nng_strerror(rv));
        return;
    }

    LOG_INFO("MSG Server listening on {}", url);
    if ((rv = nng_setopt_ms(msg_sock, NNG_OPT_SENDTIMEO, 2000)) != 0) {
        LOG_ERROR("nng_setopt_ms: {}", nng_strerror(rv));
        return;
    }

    while (gIsListening) {
        unique_lock<mutex> lock(gMutex);
        if (gCV.wait_for(lock, chrono::milliseconds(1000), []() { return !gMsgQueue.empty(); })) {
            while (!gMsgQueue.empty()) {
                auto wxmsg             = gMsgQueue.front();
                rsp.msg.wxmsg.is_self  = wxmsg.is_self;
                rsp.msg.wxmsg.is_group = wxmsg.is_group;
                rsp.msg.wxmsg.type     = wxmsg.type;
                rsp.msg.wxmsg.id       = (char *)wxmsg.id.c_str();
                rsp.msg.wxmsg.xml      = (char *)wxmsg.xml.c_str();
                rsp.msg.wxmsg.sender   = (char *)wxmsg.sender.c_str();
                rsp.msg.wxmsg.roomid   = (char *)wxmsg.roomid.c_str();
                rsp.msg.wxmsg.content  = (char *)wxmsg.content.c_str();
                rsp.msg.wxmsg.thumb    = (char *)wxmsg.thumb.c_str();
                rsp.msg.wxmsg.extra    = (char *)wxmsg.extra.c_str();
                gMsgQueue.pop();
                LOG_DEBUG("Recv msg: {}", wxmsg.content);
                pb_ostream_t stream = pb_ostream_from_buffer(buffer, G_BUF_SIZE);
                if (!pb_encode(&stream, Response_fields, &rsp)) {
                    LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
                    continue;
                }

                rv = nng_send(msg_sock, buffer, stream.bytes_written, 0);
                if (rv != 0) {
                    LOG_ERROR("nng_send: {}", nng_strerror(rv));
                }
                LOG_DEBUG("Send data length {}", stream.bytes_written);
            }
        }
    }
    nng_close(msg_sock);
}

bool func_enable_recv_txt(uint8_t *out, size_t *len)
{
    Response rsp   = Response_init_default;
    rsp.func       = Functions_FUNC_ENABLE_RECV_TXT;
    rsp.which_msg  = Response_status_tag;
    rsp.msg.status = -1;

    ListenMessage();
    HANDLE msgThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)PushMessage, NULL, NULL, NULL);
    if (msgThread != 0) {
        CloseHandle(msgThread);
        rsp.msg.status = 0;
    }

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_disable_recv_txt(uint8_t *out, size_t *len)
{
    Response rsp   = Response_init_default;
    rsp.func       = Functions_FUNC_DISABLE_RECV_TXT;
    rsp.which_msg  = Response_status_tag;
    rsp.msg.status = 0;

    UnListenMessage(); // 可能需要1秒之后才能退出，见 PushMessage

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_exec_db_query(char *db, char *sql, uint8_t *out, size_t *len)
{
    Response rsp  = Response_init_default;
    rsp.func      = Functions_FUNC_GET_DB_TABLES;
    rsp.which_msg = Response_rows_tag;

    DbRows_t rows                  = ExecDbQuery(db, sql);
    rsp.msg.rows.rows.arg          = &rows;
    rsp.msg.rows.rows.funcs.encode = encode_rows;

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_accept_friend(char *v3, char *v4, int32_t scene, uint8_t *out, size_t *len)
{
    Response rsp   = Response_init_default;
    rsp.func       = Functions_FUNC_SEND_IMG;
    rsp.which_msg  = Response_status_tag;
    rsp.msg.status = 0;

    if ((v3 == NULL) || (v4 == NULL)) {
        rsp.msg.status = -1;
        LOG_ERROR("Empty V3 or V4.");
    } else {
        rsp.msg.status = AcceptNewFriend(v3, v4, scene);
        if (rsp.msg.status != 1) {
            LOG_ERROR("AcceptNewFriend failed: {}", rsp.msg.status);
        }
    }

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_add_room_members(char *roomid, char *wxids, uint8_t *out, size_t *len)
{
    Response rsp   = Response_init_default;
    rsp.func       = Functions_FUNC_ADD_ROOM_MEMBERS;
    rsp.which_msg  = Response_status_tag;
    rsp.msg.status = 0;

    rsp.msg.status = AddChatroomMember(roomid, wxids);
    if (rsp.msg.status != 1) {
        LOG_ERROR("AddChatroomMember failed: {}", rsp.msg.status);
    }

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_receive_transfer(char *wxid, char *transferid, uint8_t *out, size_t *len)
{
    Response rsp   = Response_init_default;
    rsp.func       = Functions_FUNC_RECV_TRANSFER;
    rsp.which_msg  = Response_status_tag;
    rsp.msg.status = 0;

    rsp.msg.status = ReceiveTransfer(wxid, transferid);
    if (rsp.msg.status != 1) {
        LOG_ERROR("AddChatroomMember failed: {}", rsp.msg.status);
    }

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

bool func_decrypt_image(char *src, char *dst, uint8_t *out, size_t *len)
{
    Response rsp   = Response_init_default;
    rsp.func       = Functions_FUNC_DECRYPT_IMAGE;
    rsp.which_msg  = Response_status_tag;
    rsp.msg.status = 0;

    rsp.msg.status = (int)DecryptImage(src, dst);
    if (rsp.msg.status != 1) {
        LOG_ERROR("DecryptImage failed.");
    }

    pb_ostream_t stream = pb_ostream_from_buffer(out, *len);
    if (!pb_encode(&stream, Response_fields, &rsp)) {
        LOG_ERROR("Encoding failed: {}", PB_GET_ERROR(&stream));
        return false;
    }
    *len = stream.bytes_written;

    return true;
}

static bool dispatcher(uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len)
{
    bool ret            = false;
    Request req         = Request_init_default;
    pb_istream_t stream = pb_istream_from_buffer(in, in_len);
    if (!pb_decode(&stream, Request_fields, &req)) {
        LOG_ERROR("Decoding failed: {}", PB_GET_ERROR(&stream));
        pb_release(Request_fields, &req);
        return false;
    }

    LOG_DEBUG("Func: {:#x} Data: {}", (uint8_t)req.func, in_len);
    switch (req.func) {
        case Functions_FUNC_IS_LOGIN: {
            LOG_DEBUG("[Functions_FUNC_IS_LOGIN]");
            ret = func_is_login(out, out_len);
            break;
        }
#if 0
        case Functions_FUNC_GET_SELF_WXID: {
            LOG_DEBUG("[Functions_FUNC_GET_SELF_WXID]");
            ret = func_get_self_wxid(out, out_len);
            break;
        }
        case Functions_FUNC_GET_MSG_TYPES: {
            LOG_DEBUG("[Functions_FUNC_GET_MSG_TYPES]");
            ret = func_get_msg_types(out, out_len);
            break;
        }
        case Functions_FUNC_GET_CONTACTS: {
            LOG_DEBUG("[Functions_FUNC_GET_CONTACTS]");
            ret = func_get_contacts(out, out_len);
            break;
        }
        case Functions_FUNC_GET_DB_NAMES: {
            LOG_DEBUG("[Functions_FUNC_GET_DB_NAMES]");
            ret = func_get_db_names(out, out_len);
            break;
        }
        case Functions_FUNC_GET_DB_TABLES: {
            LOG_DEBUG("[Functions_FUNC_GET_DB_TABLES]");
            ret = func_get_db_tables(req.msg.str, out, out_len);
            break;
        }
        case Functions_FUNC_GET_USER_INFO: {
            LOG_DEBUG("[Functions_FUNC_GET_USER_INFO]");
            ret = func_get_user_info(out, out_len);
            break;
        }
        case Functions_FUNC_SEND_TXT: {
            LOG_DEBUG("[Functions_FUNC_SEND_TXT]");
            ret = func_send_txt(req.msg.txt, out, out_len);
            break;
        }
        case Functions_FUNC_SEND_IMG: {
            LOG_DEBUG("[Functions_FUNC_SEND_IMG]");
            ret = func_send_img(req.msg.file.path, req.msg.file.receiver, out, out_len);
            break;
        }
        case Functions_FUNC_SEND_FILE: {
            LOG_DEBUG("[Functions_FUNC_SEND_FILE]");
            ret = func_send_file(req.msg.file.path, req.msg.file.receiver, out, out_len);
            break;
        }
        case Functions_FUNC_SEND_XML: {
            LOG_DEBUG("[Functions_FUNC_SEND_XML]");
            ret = func_send_xml(req.msg.xml, out, out_len);
            break;
        }
        case Functions_FUNC_SEND_EMOTION: {
            LOG_DEBUG("[Functions_FUNC_SEND_EMOTION]");
            ret = func_send_emotion(req.msg.file.path, req.msg.file.receiver, out, out_len);
            break;
        }
        case Functions_FUNC_ENABLE_RECV_TXT: {
            LOG_DEBUG("[Functions_FUNC_ENABLE_RECV_TXT]");
            ret = func_enable_recv_txt(out, out_len);
            break;
        }
        case Functions_FUNC_DISABLE_RECV_TXT: {
            LOG_DEBUG("[Functions_FUNC_DISABLE_RECV_TXT]");
            ret = func_disable_recv_txt(out, out_len);
            break;
        }
        case Functions_FUNC_EXEC_DB_QUERY: {
            LOG_DEBUG("[Functions_FUNC_EXEC_DB_QUERY]");
            ret = func_exec_db_query(req.msg.query.db, req.msg.query.sql, out, out_len);
            break;
        }
        case Functions_FUNC_ACCEPT_FRIEND: {
            LOG_DEBUG("[Functions_FUNC_ACCEPT_FRIEND]");
            ret = func_accept_friend(req.msg.v.v3, req.msg.v.v4, req.msg.v.scene, out, out_len);
            break;
        }
        case Functions_FUNC_ADD_ROOM_MEMBERS: {
            LOG_DEBUG("[Functions_FUNC_ADD_ROOM_MEMBERS]");
            ret = func_add_room_members(req.msg.m.roomid, req.msg.m.wxids, out, out_len);
            break;
        }
        case Functions_FUNC_RECV_TRANSFER: {
            LOG_DEBUG("[Functions_FUNC_RECV_TRANSFER]");
            ret = func_receive_transfer(req.msg.tf.wxid, req.msg.tf.tid, out, out_len);
            break;
        }
        case Functions_FUNC_DECRYPT_IMAGE: {
            LOG_DEBUG("[FUNCTIONS_FUNC_DECRYPT_IMAGE]");
            ret = func_decrypt_image(req.msg.dec.src, req.msg.dec.dst, out, out_len);
            break;
        }
#endif
        default: {
            LOG_ERROR("[UNKNOW FUNCTION]");
            break;
        }
    }
    pb_release(Request_fields, &req);
    return ret;
}

static int RunServer()
{
    int rv                 = 0;
    char url[URL_SIZE + 1] = { 0 };
    sprintf_s(url, URL_SIZE, "%s:%d", BASE_URL, lport);
    if ((rv = nng_pair1_open(&sock)) != 0) {
        LOG_ERROR("nng_pair0_open error {}", nng_strerror(rv));
        return rv;
    }

    if ((rv = nng_listen(sock, (char *)url, NULL, 0)) != 0) {
        LOG_ERROR("nng_listen error {}", nng_strerror(rv));
        return rv;
    }

    LOG_INFO("CMD Server listening on {}", (char *)url);
    if ((rv = nng_setopt_ms(sock, NNG_OPT_SENDTIMEO, 1000)) != 0) {
        LOG_ERROR("nng_setopt_ms error: {}", nng_strerror(rv));
        return rv;
    }

    lIsRunning = true;
    while (lIsRunning) {
        uint8_t *in = NULL;
        size_t in_len, out_len = G_BUF_SIZE;
        if ((rv = nng_recv(sock, &in, &in_len, NNG_FLAG_ALLOC)) != 0) {
            LOG_ERROR("nng_recv error: {}", nng_strerror(rv));
            break;
        }

        // LOG_BUFFER(in, in_len);
        if (dispatcher(in, in_len, gBuffer, &out_len)) {
            LOG_DEBUG("Send data length {}", out_len);
            // LOG_BUFFER(gBuffer, out_len);
            rv = nng_send(sock, gBuffer, out_len, 0);
            if (rv != 0) {
                LOG_ERROR("nng_send: {}", nng_strerror(rv));
            }

        } else {
            // Error
            LOG_ERROR("Dispatcher failed...");
            rv = nng_send(sock, gBuffer, 0, 0);
            if (rv != 0) {
                LOG_ERROR("nng_send: {}", nng_strerror(rv));
            }
            // break;
        }
        nng_free(in, in_len);
    }
    LOG_DEBUG("Leave RunServer");
    return rv;
}

int RpcStartServer(int port)
{
    if (lIsRunning) {
        return 0;
    }

    lport = port;

    HANDLE rpcThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)RunServer, NULL, NULL, &lThreadId);
    if (rpcThread != 0) {
        CloseHandle(rpcThread);
    }

    return 0;
}

int RpcStopServer()
{
    if (lIsRunning) {
        nng_close(sock);
        UnListenMessage();
        lIsRunning = false;
        Sleep(1000);
        LOG_INFO("Server stoped.");
    }
    return 0;
}
