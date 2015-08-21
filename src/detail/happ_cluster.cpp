#include <algorithm>
#include <cstdio>
#include <random>
#include <sstream>
#include <ctime>

#include "detail/crc16.h"
#include "detail/happ_cluster.h"

namespace hiredis {
    namespace happ {
        namespace detail {
            static int random() {
#if defined(__cplusplus) && __cplusplus >= 201103L
                static std::mt19937 g;
                return static_cast<int>(g());
#else
                static bool inited = false;
                if (!inited) {
                    inited = true;
                    srand(time(NULL));
                }

                return rand();
#endif
            }
        }

        cluster::cluster(): slot_flag(slot_status::INVALID) {
            conf.log_fn_debug = conf.log_fn_info = NULL;
            conf.log_buffer = NULL;
            conf.log_max_size = 0;
            conf.timer_interval_sec = HIREDIS_HAPP_TIMER_INTERVAL_SEC;
            conf.timer_interval_usec = HIREDIS_HAPP_TIMER_INTERVAL_USEC;

            for (int i = 0; i < HIREDIS_HAPP_SLOT_NUMBER; ++ i) {
                slots[i].index = i;
            }

            memset(&callbacks, 0, sizeof(callbacks));

            timer_actions.last_update_sec = 0;
            timer_actions.last_update_usec = 0;
        }

        cluster::~cluster() {
            reset();
        }

        int cluster::init(const std::string& ip, uint16_t port) {
            connection::set_key(conf.init_connection, ip, port);
            
            return error_code::REDIS_HAPP_OK;
        }

        int cluster::start() {
            reload_slots();
            return error_code::REDIS_HAPP_OK;
        }

        int cluster::reset() {
            std::vector<redisAsyncContext*> all_contexts;
            all_contexts.reserve(connections.size());

            // ��Ԥ���������ӣ��ٹر�
            for (connection_map_t::iterator it = connections.begin(); it != connections.end(); ++it) {
                if (NULL != it->second.get_context()) {
                    all_contexts.push_back(it->second.get_context());
                }
            }

            for (size_t i = 0; i < all_contexts.size(); ++ i) {
                redisAsyncDisconnect(all_contexts[i]);
            }

            // �ͷ�slot pending list
            while(!slot_pending.empty()) {
                cmd_t* cmd = slot_pending.front();
                slot_pending.pop_front();

                call_cmd(cmd, error_code::REDIS_HAPP_SLOT_UNAVAILABLE, NULL, NULL);
                destroy_cmd(cmd);
            }

            for (int i = 0; i < HIREDIS_HAPP_SLOT_NUMBER; ++i) {
                slots[i].hosts.clear();
            }
            slot_flag = slot_status::INVALID;

            // �ͷ�timer pending list
            while(!timer_actions.timer_pending.empty()) {
                cmd_t* cmd = timer_actions.timer_pending.front().cmd;
                timer_actions.timer_pending.pop_front();

                call_cmd(cmd, error_code::REDIS_HAPP_TIMEOUT, NULL, NULL);
                destroy_cmd(cmd);
            }
            timer_actions.last_update_sec = 0;
            timer_actions.last_update_usec = 0;

            // log buffer
            if (NULL != conf.log_buffer) {
                free(conf.log_buffer);
            }

            return 0;
        }

        int cluster::exec(const char* key, size_t ks, cmd_t::callback_fn_t cbk, void* priv_data, int argc, const char** argv, const size_t* argvlen) {
            cmd_t* cmd = create_cmd(cbk, priv_data);
            if (NULL == cmd) {
                return error_code::REDIS_HAPP_CREATE;
            }

            int len = cmd->format(argc, argv, argvlen);
            if (len <= 0) {
                log_info("format cmd with argc=%d failed", argc);
                destroy_cmd(cmd);
                return error_code::REDIS_HAPP_CREATE;
            }

            return exec(key, ks, cmd);
        }

        int cluster::exec(const char* key, size_t ks, cmd_t::callback_fn_t cbk, void* priv_data, const char* fmt, ...) {
            cmd_t* cmd = create_cmd(cbk, priv_data);
            if (NULL == cmd) {
                return error_code::REDIS_HAPP_CREATE;
            }

            va_list ap;
            va_start(ap, fmt);
            int len = cmd->format(fmt, ap);
            va_end(ap);
            if (len <= 0) {
                log_info("format cmd with format=%s failed", fmt);
                destroy_cmd(cmd);
                return error_code::REDIS_HAPP_CREATE;
            }

            return exec(key, ks, cmd);
        }

        int cluster::exec(const char* key, size_t ks, cmd_t::callback_fn_t cbk, void* priv_data, const char* fmt, va_list ap) {
            cmd_t* cmd = create_cmd(cbk, priv_data);
            if (NULL == cmd) {
                return error_code::REDIS_HAPP_CREATE;
            }

            int len = cmd->format(fmt, ap);
            if (len <= 0) {
                log_info("format cmd with format=%s failed", fmt);
                destroy_cmd(cmd);
                return error_code::REDIS_HAPP_CREATE;
            }

            return exec(key, ks, cmd);
        }

        int cluster::exec(const char* key, size_t ks, cmd_t* cmd) {
            if (NULL == cmd) {
                return error_code::REDIS_HAPP_PARAM;
            }

            // ��Ҫ������ת��cmd_t������Ȩ
            if (NULL != key && 0 != ks) {
                cmd->engine.slot = static_cast<int>(crc16(key, ks));
            }

            // ttl Ԥ�ж�
            if (0 == cmd->ttl) {
                log_debug("cmd at slot %d ttl expired", cmd->engine.slot);
                call_cmd(cmd, error_code::REDIS_HAPP_TTL, NULL, NULL);
                destroy_cmd(cmd);
                return error_code::REDIS_HAPP_TTL;
            }

            // update slot
            if (slot_status::INVALID == slot_flag || slot_status::UPDATING == slot_flag) {
                log_debug("transfer cmd at slot %d to slot update pending list", cmd->engine.slot);
                slot_pending.push_back(cmd);

                reload_slots();
                return 0;
            }

            // ָ���������ȡ��������ַ
            const connection::key_t* conn_key = get_slot_master(cmd->engine.slot);

            if (NULL == conn_key) {
                log_info("get connect of slot %d failed", cmd->engine.slot);
                call_cmd(cmd, error_code::REDIS_HAPP_CONNECTION, NULL, NULL);
                return error_code::REDIS_HAPP_CONNECTION;
            }

            // ת������������
            connection_t* conn_inst = get_connection(conn_key->name);
            if (NULL == conn_inst) {
                conn_inst = make_connection(*conn_key);
            }

            if (NULL == conn_inst) {
                log_info("connect to %s failed", conn_key->name.c_str());

                call_cmd(cmd, error_code::REDIS_HAPP_CONNECTION, NULL, NULL);
                destroy_cmd(cmd);

                return error_code::REDIS_HAPP_CREATE;
            }

            return exec(conn_inst, cmd);
        }

        int cluster::exec(connection_t* conn, cmd_t* cmd) {
            if (NULL == cmd) {
                return error_code::REDIS_HAPP_PARAM;
            }

            // ttl ��ʽ�ж�
            if (0 == cmd->ttl) {
                log_debug("cmd at slot %d ttl expired", cmd->engine.slot);
                call_cmd(cmd, error_code::REDIS_HAPP_TTL, NULL, NULL);
                destroy_cmd(cmd);
                return error_code::REDIS_HAPP_TTL;
            }

            // ttl
            --cmd->ttl;

            if (NULL == conn) {
                if (NULL != cmd) {
                    cmd->call_reply(error_code::REDIS_HAPP_CONNECTION, NULL, NULL);
                    destroy_cmd(cmd);
                }

                return error_code::REDIS_HAPP_PARAM;
            }

            // ��ѭ���߼��ذ�����
            int res = conn->redis_cmd(cmd, on_reply_wrapper);

            // hiredis�Ĵ��룬��������رպ��������᷵�س���
            // ������Щ�����Ӧ��ֱ�ӳ���ص�
            if (REDIS_OK != res) {
                cmd->call_reply(error_code::REDIS_HAPP_HIREDIS, conn->get_context(), NULL);
                destroy_cmd(cmd);
            }

            log_debug("exec cmd at slot %d, connection %s", cmd->engine.slot, conn->get_key().name.c_str());
            return error_code::REDIS_HAPP_OK;
        }

        int cluster::retry(cmd_t* cmd, connection_t* conn) {
            // ���Դ���������ֱ������
            if(NULL == cmd) {
                return error_code::REDIS_HAPP_PARAM;
            }

            if (false == is_timer_active() || cmd->ttl > HIREDIS_HAPP_TTL / 2) {
                if (NULL == conn) {
                    return exec(NULL, 0, cmd);
                } else {
                    return exec(conn, cmd);
                }
            }

            // ���Դ����϶����һ������
            // �ӳ����Ե������¼������Ϣ����Ϊ���ܵ�ʱ�������Ѿ���ʧ
            add_timer_cmd(cmd);
            return 0;
        }

        bool cluster::reload_slots() {
            if (slot_status::UPDATING == slot_flag) {
                return false;
            }

            const connection::key_t* conn_key = get_slot_master(-1);
            if (NULL == conn_key) {
                return false;
            }

            connection_t* conn = get_connection(conn_key->name);
            if (NULL == conn) {
                slot_flag = slot_status::INVALID;
                make_connection(*conn_key);
                return true;
            }

            // ����Ҫ��ԭʼ�ӿڣ���Ϊexec_cmd�����Ϣ�Ӵ�ִ�ж�����
            redisAsyncCommand(const_cast<redisAsyncContext*>(conn->get_context()), on_reply_update_slot, NULL, "CLUSTER SLOTS");
            slot_flag = slot_status::UPDATING;

            return true;
        }

        const connection::key_t* cluster::get_slot_master(int index) {
            if (index >= 0 && index < HIREDIS_HAPP_SLOT_NUMBER && !slots[index].hosts.empty()) {
                return &slots[index].hosts.front();
            }

            // �����ȡ��ַ
            index = (detail::random() & 0xFFFF) % HIREDIS_HAPP_SLOT_NUMBER;
            if (slots[index].hosts.empty()) {
                return &conf.init_connection;
            }

            return &slots[index].hosts.front();
        }

        const cluster::connection_t* cluster::get_connection(const std::string& key) const {
            connection_map_t::const_iterator it = connections.find(key);
            if (it == connections.end()) {
                return NULL;
            }

            return &it->second;
        }

        cluster::connection_t* cluster::get_connection(const std::string& key) {
            connection_map_t::iterator it = connections.find(key);
            if (it == connections.end()) {
                return NULL;
            }

            return &it->second;
        }

        const cluster::connection_t* cluster::get_connection(const std::string& ip, uint16_t port) const {
            return get_connection(connection::make_name(ip, port));
        }

        cluster::connection_t* cluster::get_connection(const std::string& ip, uint16_t port) {
            return get_connection(connection::make_name(ip, port));
        }

        cluster::connection_t* cluster::make_connection(const connection::key_t& key) {
            holder_t h;
            if (connections.find(key.name) == connections.end()) {
                return NULL;
            }

            redisAsyncContext* c = redisAsyncConnect(key.ip.c_str(), static_cast<int>(key.port));
            if (NULL == c || c->err) {
                log_info("redis connect to %s failed, msg: %s", key.name.c_str(), NULL == c? "none": c->errstr);
                return NULL;
            }

            h.clu = this;
            redisAsyncSetConnectCallback(c, on_connected_wrapper);
            redisAsyncSetDisconnectCallback(c, on_disconnected_wrapper);

            connection_t& ret = connections[key.name];
            ret.init(h, key);
            ret.set_connecting(c);

            c->data = &ret;

            // event callback
            if (callbacks.on_connect) {
                callbacks.on_connect(this, &ret);
            }

            log_debug("redis make connection to %s ", key.name.c_str());
            return &ret;
        }

        bool cluster::release_connection(const connection::key_t& key, bool close_fd, int status) {
            connection_map_t::iterator it = connections.find(key.name);
            if (connections.end() == it) {
                return false;
            }

            std::list<cmd_exec*> pending_list;
            it->second.set_disconnected(&pending_list, close_fd);

            if(callbacks.on_disconnected) {
                callbacks.on_disconnected(this, &it->second, it->second.get_context(), status);
            }

            log_debug("release connection %s", key.name.c_str());
            connections.erase(it);


            // ����cmd
            for (std::list<cmd_exec*>::iterator it_cmd; it_cmd != pending_list.end(); ++it_cmd) {
                retry(*it_cmd);
            }
            
            return true;
        }

        cluster::onconnect_fn_t cluster::set_on_connect(onconnect_fn_t cbk) {
            using std::swap;
            swap(cbk, callbacks.on_connect);
            return cbk;
        }

        cluster::onconnected_fn_t cluster::set_on_connected(onconnected_fn_t cbk) {
            using std::swap;
            swap(cbk, callbacks.on_connected);
            return cbk;
        }

        cluster::ondisconnected_fn_t cluster::set_on_disconnected(ondisconnected_fn_t cbk) {
            using std::swap;
            swap(cbk, callbacks.on_disconnected);
            return cbk;
        }

        bool cluster::is_timer_active() const {
            return (timer_actions.last_update_sec != 0 || timer_actions.last_update_usec != 0) &&
                (conf.timer_interval_sec > 0 || conf.timer_interval_usec > 0);
        }

        void cluster::set_timer_interval(time_t sec, time_t usec) {
            conf.timer_interval_sec = sec;
            conf.timer_interval_usec = usec;
        }

        void cluster::add_timer_cmd(cmd_t* cmd) {
            if (NULL == cmd) {
                return;
            }

            timer_actions.timer_pending.push_back(timer_t::delay_t());
            timer_t::delay_t& d = timer_actions.timer_pending.back();
            d.sec = timer_actions.last_update_sec + conf.timer_interval_sec;
            d.usec = timer_actions.last_update_usec + conf.timer_interval_usec;
            d.cmd = cmd;
        }

        int cluster::proc(time_t sec, time_t usec) {
            int ret = 0;

            timer_actions.last_update_sec = sec;
            timer_actions.last_update_usec = usec;

            while (!timer_actions.timer_pending.empty()) {
                timer_t::delay_t& rd = timer_actions.timer_pending.front();
                if (rd.sec > sec || (rd.sec == sec && rd.usec > usec)) {
                    break;
                }


                timer_t::delay_t d = rd;
                timer_actions.timer_pending.pop_front();

                exec(NULL, 0, d.cmd);

                ++ret;
            }

            return ret;
        }

        cluster::cmd_t* cluster::create_cmd(cmd_t::callback_fn_t cbk, void* pridata) {
            holder_t h;
            h.clu = this;
            cmd_t* ret = cmd_t::create(h, cbk, pridata);
            return ret;
        }

        void cluster::destroy_cmd(cmd_t* c) {
            if (NULL == c) {
                log_debug("can not destroy null cmd");
                return;
            }

            // ��ʧ����
            if (NULL != c->callback) {
                call_cmd(c, error_code::REDIS_HAPP_UNKNOWD, NULL, NULL);
            }

            cmd_t::destroy(c);
        }

        int cluster::call_cmd(cmd_t* c, int err, redisAsyncContext* context, void* reply) {
            if (NULL == c) {
                log_debug("can not call cmd without cmd object");
                return error_code::REDIS_HAPP_UNKNOWD;
            }

            return c->call_reply(err, context, reply);
        }

        void cluster::set_log_writer(log_fn_t info_fn, log_fn_t debug_fn, size_t max_size) {
            using std::swap;
            conf.log_fn_info = info_fn;
            conf.log_fn_debug = debug_fn;
            conf.log_max_size = max_size;

            if (NULL != conf.log_buffer) {
                free(conf.log_buffer);
                conf.log_buffer = NULL;
            }
        }

        void cluster::on_reply_wrapper(redisAsyncContext* c, void* r, void* privdata) {
            connection_t* conn = reinterpret_cast<connection_t*>(c->data);
            cmd_t* cmd = reinterpret_cast<cmd_t*>(privdata);
            cluster* self = conn->get_holder().clu;

            if (REDIS_ERR_IO == c->err && REDIS_ERR_EOF == c->err) {
                self->log_debug("redis reply context err %d and will retry, %s", c->err, c->errstr);
                // �������������
                self->retry(cmd);
                return;
            }

            if (REDIS_OK != c->err || NULL == r) {
                self->log_debug("redis reply context err %d and abort, %s", c->err, NULL == c->errstr? "none": c->errstr);
                // �������������ϴ���
                conn->call_reply(cmd, r);
                return;
            }

            redisReply* reply = reinterpret_cast<redisReply*>(r);

            // ������
            if (REDIS_REPLY_ERROR == reply->type) {
                int slot_index = 0;
                char addr[260] = { 0 };

                // ��� MOVED��ASK��CLUSTERDOWNָ��
                if(0 == HIREDIS_HAPP_STRNCASE_CMP("ASK", reply->str, 3)) {
                    self->log_debug("%s", reply->str);
                    // ����ASK��Ŀ��connection
                    HIREDIS_HAPP_SSCANF(reply->str + 4, " %d %s", &slot_index, addr);
                    std::string ip;
                    uint16_t port;
                    if (connection::pick_name(addr, ip, port)) {
                        connection::key_t conn_key;
                        connection::set_key(conn_key, ip, port);

                        // ASKING ����
                        connection_t* conn = self->get_connection(conn_key.name);
                        if (NULL == conn) {
                            conn = self->make_connection(conn_key);
                        }

                        // cmdת�Ƶ��µ�connection��������ɺ�ִ��
                        if (NULL != conn) {
                            if(REDIS_OK == redisAsyncCommand(conn->get_context(), on_reply_asking, cmd, "ASKING")) {
                                return;
                            }
                        }

                        return;
                    }
                } else if (0 == HIREDIS_HAPP_STRNCASE_CMP("MOVED", reply->str, 5)) {
                    self->log_debug("%s", reply->str);

                    HIREDIS_HAPP_SSCANF(reply->str + 6, " %d %s", &slot_index, addr);
                    std::string ip;
                    uint16_t port;
                    if (connection::pick_name(addr, ip, port)) {
                        // ����һ��slot
                        self->slots[slot_index].hosts.clear();
                        self->slots[slot_index].hosts.push_back(connection::key_t());
                        connection::set_key(self->slots[slot_index].hosts.back(), ip, port);

                        // retry
                        self->retry(cmd);

                        // ������ȡslot�б�
                        // TODO �����Ƿ�Ҫǿ����ȡslots�б�
                        // �������ȡ���ܶ�ʧ�ӽڵ���Ϣ��������ȡ�Ļ�Ǩ�ƹ����п��ܻᵼ�¸��¶�Σ�
                        // ���Ҹ���slotsҲ��һ���ȽϺķ�CPU�Ĳ�����16384��list����պ͸��ƣ�
                        self->reload_slots();
                        return;
                    } else {
                        self->slot_flag = slot_status::INVALID;
                    }
                } else if (0 == HIREDIS_HAPP_STRNCASE_CMP("CLUSTERDOWN", reply->str, 11)) {
                    self->log_info("cluster down reset all connection, cmd and replys");
                    conn->call_reply(cmd, r);
                    self->reset();
                    return;
                }

                self->log_debug("redis reply errorand abort, msg: %s", NULL == reply->str? "none": reply->str);
                // �������������ϴ���
                conn->call_reply(cmd, r);
                return;
            }

            // �����ص�
            conn->call_reply(cmd, r);
        }

        void cluster::on_reply_update_slot(redisAsyncContext* c, void* r, void* privdata) {
            redisReply* reply = reinterpret_cast<redisReply*>(r);
            connection_t* conn = reinterpret_cast<connection_t*>(c->data);
            cluster* self = conn->get_holder().clu;

            // ����������ȡ
            if (NULL == reply || reply->elements <= 0 || REDIS_REPLY_ARRAY != reply->element[0]->type) {
                self->slot_flag = slot_status::INVALID;

                if (!self->slot_pending.empty()) {
                    self->log_info("update slots failed and try to retry again.");
                    self->reload_slots();
                } else {
                    self->log_info("update slots failed and will retry later.");
                }
                
                return;
            }

            // clear and reset slots ... 
            for (size_t i = 0; i < HIREDIS_HAPP_SLOT_NUMBER; ++ i) {
                self->slots[i].hosts.clear();
            }

            for (size_t i = 0; i < reply->elements; ++ i) {
                redisReply* slot_node = reply->element[i];
                if (slot_node->elements >= 3) {
                    long long si = slot_node->element[0]->integer;
                    long long ei = slot_node->element[1]->integer;

                    std::vector<connection::key_t> hosts;
                    for (size_t j = 2; j < slot_node->elements; ++ j) {
                        redisReply* addr = slot_node->element[i];
                        if (2 == addr->elements && REDIS_REPLY_STRING == addr->element[0]->type && REDIS_REPLY_INTEGER == addr->element[1]->type) {
                            hosts.push_back(connection::key_t());
                            connection::set_key(
                                hosts.back(),
                                addr->element[0]->str,
                                static_cast<uint16_t>(addr->element[1]->integer)
                            );
                        }
                        
                    }

                    // 16384�θ���
                    for (; si <= ei; ++ si) {
                        self->slots[si].hosts = hosts;
                    }
                }
            }

            // ִ�д�ִ�ж���
            while(!self->slot_pending.empty()) {
                cmd_t* cmd = self->slot_pending.front();
                self->slot_pending.pop_front();
                self->retry(cmd);
            }
            self->log_info("update %d slots done",static_cast<int>(reply->elements));
        }

        void cluster::on_reply_asking(redisAsyncContext* c, void* r, void* privdata) {
            cmd_t* cmd = reinterpret_cast<cmd_t*>(privdata);
            redisReply* reply = reinterpret_cast<redisReply*>(r);
            connection_t* conn = reinterpret_cast<connection_t*>(c->data);
            cluster* self = conn->get_holder().clu;


            if (REDIS_ERR_IO == c->err && REDIS_ERR_EOF == c->err) {
                self->log_debug("redis asking err %d and will retry, %s", c->err, c->errstr);
                // �������������
                self->retry(cmd);
                return;
            }

            if (REDIS_OK != c->err || NULL == r) {
                self->log_debug("redis asking err %d and abort, %s", c->err, NULL == c->errstr ? "none" : c->errstr);
                // �������������ϴ���
                conn->call_reply(cmd, r);
                return;
            }

            if (NULL != reply->str && 0 == HIREDIS_HAPP_STRNCASE_CMP("OK", reply->str, 2)) {
                self->retry(cmd, conn);
                return;
            }

            self->log_debug("redis reply asking err %d and abort, %s", reply->type, NULL == reply->str ? "none" : reply->str);
            // �������������ϴ���
            conn->call_reply(cmd, r);
        }

        void cluster::on_connected_wrapper(const struct redisAsyncContext* c, int status) {
            connection_t* conn = reinterpret_cast<connection_t*>(c->data);
            cluster* self = conn->get_holder().clu;
            
            // event callback
            if (self->callbacks.on_connected) {
                self->callbacks.on_connected(self, conn, c, status);
            }

            // ʧ�����ͷ���Դ
            if (REDIS_OK != status) {
                self->log_debug("connect to %s failed, status: %d, msg: %s", conn->get_key().name.c_str(), status, c->errstr);
                self->release_connection(conn->get_key(), false, status);
            } else {
                // ִ��pending����
                std::list<cmd_exec*> pending_list;
                conn->set_connected(pending_list);
                for (std::list<cmd_exec*>::iterator it = pending_list.begin(); it != pending_list.end(); ++ it) {
                    self->retry(*it);
                }

                self->log_debug("connect to %s success", conn->get_key().name.c_str());

                // ����slot
                if (slot_status::INVALID == self->slot_flag) {
                    self->reload_slots();
                }
            }

        }

        void cluster::on_disconnected_wrapper(const struct redisAsyncContext* c, int status) {
            connection_t* conn = reinterpret_cast<connection_t*>(c->data);
            cluster* self = conn->get_holder().clu;
            // �ͷ���Դ
            self->release_connection(conn->get_key(), false, status);
        }

        void cluster::dump(std::ostream& out, redisReply* reply, int ident) {
            if (NULL == reply) {
                return;
            }

            // TODO dump reply
            switch(reply->type) {
            case REDIS_REPLY_NIL: {
                out << "[NIL]"<< std::endl;
                break;
            }
            case REDIS_REPLY_STATUS: {
                out << "[STATUS]: "<< reply->str << std::endl;
                break;
            }
            case REDIS_REPLY_ERROR: {
                out << "[ERROR]: " << reply->str << std::endl;
                break;
            }
            case REDIS_REPLY_INTEGER: {
                out << reply->integer << std::endl;
                break;
            }
            case REDIS_REPLY_STRING: {
                out << reply->str << std::endl;
                break;
            }
            case REDIS_REPLY_ARRAY: {
                std::string ident_str;
                ident_str.assign(static_cast<size_t>(ident), ' ');

                out << "[ARRAY]: " << std::endl;
                for (size_t i = 0; i < reply->elements; ++ i) {
                    out << ident_str << out.width(7) << (i + 1) << ": ";
                    dump(out, reply->element[i], ident + 2);
                }

                break;
            }
            default: {
                log_debug("[UNKNOWN]");
                break;
            }
            }

        }

        void cluster::log_debug(const char* fmt, ...) {
            if (NULL == conf.log_fn_debug || 0 == conf.log_max_size ) {
                return;
            }

            if (NULL == conf.log_buffer) {
                conf.log_buffer = reinterpret_cast<char*>(malloc(conf.log_max_size));
            }

            va_list ap;
            va_start(ap, fmt);
            int len = c99_vsnprintf(conf.log_buffer, conf.log_max_size, fmt, ap);
            va_end(ap);

            conf.log_buffer[conf.log_max_size - 1] = 0;
            if (len > 0) {
                conf.log_buffer[len] = 0;
            }

            conf.log_fn_debug(conf.log_buffer);
        }

        void cluster::log_info(const char* fmt, ...) {
            if (NULL == conf.log_fn_info || 0 == conf.log_max_size) {
                return;
            }

            if (NULL == conf.log_buffer) {
                conf.log_buffer = reinterpret_cast<char*>(malloc(conf.log_max_size));
            }

            va_list ap;
            va_start(ap, fmt);
            int len = c99_vsnprintf(conf.log_buffer, conf.log_max_size, fmt, ap);
            va_end(ap);

            conf.log_buffer[conf.log_max_size - 1] = 0;
            if (len > 0) {
                conf.log_buffer[len] = 0;
            }

            conf.log_fn_info(conf.log_buffer);
        }
    }
}