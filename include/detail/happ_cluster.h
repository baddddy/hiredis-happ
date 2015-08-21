//
// Created by ŷ��� on 2015/08/18.
//

#ifndef HIREDIS_HAPP_HIREDIS_HAPP_CLUSTER_H
#define HIREDIS_HAPP_HIREDIS_HAPP_CLUSTER_H

#pragma once

#include <vector>
#include <list>
#include <ostream>

#include "config.h"

#include "happ_connection.h"

namespace hiredis {
    namespace happ {
        class cluster {
        public:
            typedef cmd_exec cmd_t;

            struct slot_t {
                int index;
                std::vector<connection::key_t> hosts;
            };
            typedef connection connection_t;
            typedef HIREDIS_HAPP_MAP(std::string, connection_t) connection_map_t;

            typedef std::function<void(cluster*, connection_t*)> onconnect_fn_t;
            typedef std::function<void (cluster*, connection_t*, const struct redisAsyncContext*, int status)> onconnected_fn_t;
            typedef std::function<void (cluster*, connection_t*, const struct redisAsyncContext*, int)> ondisconnected_fn_t;
            typedef std::function<void(const char*)> log_fn_t;

        private:
            cluster(const cluster&);
            cluster& operator=(const cluster&);

        public:
            cluster();
            ~cluster();

            int init(const std::string& ip, uint16_t port);

            int start();

            int reset();

            int exec(const char* key, size_t ks, cmd_t::callback_fn_t cbk, void* priv_data, int argc, const char** argv, const size_t* argvlen);

            int exec(const char* key, size_t ks, cmd_t::callback_fn_t cbk, void* priv_data, const char* fmt, ...);

            int exec(const char* key, size_t ks, cmd_t::callback_fn_t cbk, void* priv_data, const char* fmt, va_list ap);

            int exec(const char* key, size_t ks, cmd_t* cmd);

            int exec(connection_t* conn, cmd_t* cmd);

            int retry(cmd_t* cmd, connection_t* conn = NULL);

            bool reload_slots();

            const connection::key_t* get_slot_master(int index);

            const connection_t* get_connection(const std::string& key) const;
            connection_t* get_connection(const std::string& key);

            const connection_t* get_connection(const std::string& ip, uint16_t port) const;
            connection_t* get_connection(const std::string& ip, uint16_t port);

            connection_t* make_connection(const connection::key_t& key);
            bool release_connection(const connection::key_t& key, bool close_fd, int status);

            onconnect_fn_t set_on_connect(onconnect_fn_t cbk);
            onconnected_fn_t set_on_connected(onconnected_fn_t cbk);
            ondisconnected_fn_t set_on_disconnected(ondisconnected_fn_t cbk);

            bool is_timer_active() const;

            void set_timer_interval(time_t sec, time_t usec);

            void add_timer_cmd(cmd_t* cmd);

            int proc(time_t sec, time_t usec);

        private:
            cmd_t* create_cmd(cmd_t::callback_fn_t cbk, void* pridata);
            void destroy_cmd(cmd_t* c);
            int call_cmd(cmd_t* c, int err, redisAsyncContext* context, void* reply);

            //int exec_connection(const std::string& key, );

            void set_log_writer(log_fn_t info_fn, log_fn_t debug_fn, size_t max_size = 65536);


            static void on_reply_wrapper(redisAsyncContext* c, void* r, void* privdata);
            static void on_reply_update_slot(redisAsyncContext* c, void* r, void* privdata);
            static void on_reply_asking(redisAsyncContext* c, void* r, void* privdata);
            static void on_connected_wrapper(const struct redisAsyncContext*, int status);
            static void on_disconnected_wrapper(const struct redisAsyncContext*, int status);

        private:
            void dump(std::ostream& out, redisReply* reply, int ident = 0);

            void log_debug(const char* fmt, ...);

            void log_info(const char* fmt, ...);

        private:
            struct config_t {
                connection::key_t init_connection;
                log_fn_t log_fn_info;
                log_fn_t log_fn_debug;
                char* log_buffer;
                size_t log_max_size;

                time_t timer_interval_sec;
                time_t timer_interval_usec;
            };
            config_t conf;

            // ����Ϣ
            struct slot_status {
                enum type {
                    INVALID = 0,
                    UPDATING,
                    OK
                };
            };
            slot_t slots[HIREDIS_HAPP_SLOT_NUMBER];
            slot_status::type slot_flag;
            // ������Slot�����б�
            std::list<cmd_t*> slot_pending;

            // ����������Ϣ
            connection_map_t connections;


            // ��ʱ�������б�
            struct timer_t {
                time_t last_update_sec;
                time_t last_update_usec;

                struct delay_t {
                    time_t sec;
                    time_t usec;
                    cmd_t* cmd;
                };
                std::list<delay_t> timer_pending;
            };
            timer_t timer_actions;

            // �ص��ӿ��б�
            struct callback_set_t {
                onconnect_fn_t on_connect;
                onconnected_fn_t on_connected;
                ondisconnected_fn_t on_disconnected;
            };
            callback_set_t callbacks;
        };
        
    }
}

#endif //HIREDIS_HAPP_HIREDIS_HAPP_CLUSTER_H