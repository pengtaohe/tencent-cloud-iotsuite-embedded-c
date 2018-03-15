#include "tc_iot_device_config.h"
#include "tc_iot_shadow_local_data.h"
#include "tc_iot_export.h"


void _device_on_message_received(tc_iot_message_data* md);
int _tc_iot_sync_shadow_property(tc_iot_shadow_property_def properties[], bool reported, const char * doc_start, jsmntok_t * json_token, int tok_count);
bool tc_iot_device_sync_reported(tc_iot_shadow_property_def properties[]);
void operate_device(tc_iot_shadow_local_data * device);

volatile int stop;

/* 设备初始配置 */
tc_iot_shadow_config g_client_config = {
    {
        {
            /* device info*/
            TC_IOT_CONFIG_DEVICE_SECRET, TC_IOT_CONFIG_DEVICE_PRODUCT_ID,
            TC_IOT_CONFIG_DEVICE_NAME, TC_IOT_CONFIG_DEVICE_CLIENT_ID,
            TC_IOT_CONFIG_DEVICE_USER_NAME, TC_IOT_CONFIG_DEVICE_PASSWORD, 0,
        },
        TC_IOT_CONFIG_SERVER_HOST,
        TC_IOT_CONFIG_SERVER_PORT,
        TC_IOT_CONFIG_COMMAND_TIMEOUT_MS,
        TC_IOT_CONFIG_TLS_HANDSHAKE_TIMEOUT_MS,
        TC_IOT_CONFIG_KEEP_ALIVE_INTERVAL_SEC,
        TC_IOT_CONFIG_CLEAN_SESSION,
        TC_IOT_CONFIG_USE_TLS,
        TC_IOT_CONFIG_AUTO_RECONNECT,
        TC_IOT_CONFIG_ROOT_CA,
        TC_IOT_CONFIG_CLIENT_CRT,
        TC_IOT_CONFIG_CLIENT_KEY,
        NULL,
        NULL,
        0,  /* send will */
        { 
            {'M', 'Q', 'T', 'W'}, 0, {NULL, {0, NULL}}, {NULL, {0, NULL}}, 0, 0, 
        }
    },
    TC_IOT_SUB_TOPIC_DEF,
    TC_IOT_PUB_TOPIC_DEF,
    _device_on_message_received,
};


/* 影子数据 Client  */
tc_iot_shadow_client client;

/**
 * @brief get_message_ack_callback shadow_get 回调函数
 *
 * @param ack_status 回调状态，标识消息是否正常收到响应，还是已经超时等。
 * @param md 回调状态为 TC_IOT_ACK_SUCCESS 时，用来传递影子数据请求响应消息。
 * @param session_context 回调 context。
 */
void get_message_ack_callback(tc_iot_command_ack_status_e ack_status, 
        tc_iot_message_data * md , void * session_context) {

    tc_iot_mqtt_message* message = NULL;

    if (ack_status != TC_IOT_ACK_SUCCESS) {
        if (ack_status == TC_IOT_ACK_TIMEOUT) {
            LOG_ERROR("request timeout");
        }
        return;
    }

    message = md->message;
    _device_on_message_received(md);
}

/**
 * @brief report_message_ack_callback shadow_update 上报消息回调
 *
 * @param ack_status 回调状态，标识消息是否正常收到响应，还是已经超时等。
 * @param md 回调状态为 TC_IOT_ACK_SUCCESS 时，用来传递影子数据请求响应消息。
 * @param session_context 回调 context。
 */
void report_message_ack_callback(tc_iot_command_ack_status_e ack_status, 
        tc_iot_message_data * md , void * session_context) {
    tc_iot_mqtt_message* message = NULL;

    if (ack_status != TC_IOT_ACK_SUCCESS) {
        if (ack_status == TC_IOT_ACK_TIMEOUT) {
            LOG_ERROR("request timeout");
        }
        return;
    }

    message = md->message;
    LOG_TRACE("[s->c] %.*s", (int)message->payloadlen, (char*)message->payload);
}

void tc_iot_shadow_sync_to_server(tc_iot_shadow_client * p_shadow_client, tc_iot_shadow_property_def properties[], const char * desired) {
    char buffer[512];
    int buffer_len = sizeof(buffer);
    char reported[256];
    int reported_len = sizeof(reported);
    int pos = 0;
    int i = 0;
    int ret = 0;
    bool reported_changed = false;
    tc_iot_shadow_property_def * current = NULL;

    reported_changed = tc_iot_device_sync_reported(properties);
    if (!reported_changed && !desired) {
        LOG_INFO("reported status synchronized with current, and no desired, skip report.");
        return;
    }
    
    ret = tc_iot_hal_snprintf(&reported[pos], reported_len-pos, "{");
    pos += ret;

    // 对比差异，并同步数据
    for (i = 0; i < TC_IOT_PROP_TOTAL; i++ ) {
        current = &properties[i];
        if (i > 0) {
            ret = tc_iot_hal_snprintf(&reported[pos], reported_len-pos,",");
            pos += ret;
        }

        if (current->type == TC_IOT_SHADOW_TYPE_STRING) {
            ret = tc_iot_hal_snprintf(&reported[pos], reported_len-pos,"\"%s\":\"%s\"",
                    current->name, tc_iot_json_inline_escape(buffer, buffer_len, current->reported_ptr));
            pos += ret;
        } else if (current->type == TC_IOT_SHADOW_TYPE_NUMBER) {
            ret = tc_iot_hal_snprintf(&reported[pos], reported_len-pos,"\"%s\":%d",
                    current->name, *(tc_iot_shadow_number *)current->reported_ptr);
            pos += ret;
        } else if (current->type == TC_IOT_SHADOW_TYPE_BOOL) {
            ret = tc_iot_hal_snprintf(&reported[pos], reported_len-pos,"\"%s\":%s",
                    current->name, *(tc_iot_shadow_bool *)current->reported_ptr ? TC_IOT_JSON_TRUE:TC_IOT_JSON_FALSE);
            pos += ret;
        } else {
            LOG_ERROR("%s type=%d unkown.", current->name, current->type);
        }
    }
    ret = tc_iot_hal_snprintf(&reported[pos], reported_len-pos,"}");
    pos += ret;

    tc_iot_shadow_update(p_shadow_client, buffer, buffer_len, reported, desired, report_message_ack_callback, TC_IOT_CONFIG_COMMAND_TIMEOUT_MS, NULL);
    LOG_TRACE("[c->s] shadow_update\n%s", buffer);
}


/**
 * @brief _tc_iot_sync_shadow_property 根据服务端下发的影子数据，同步到本地设备状态数据，并进
 * 行上报。
 *
 * @param properties 设备状态数据
 * @param reported 同步数据类型，为 true 表示同步 reported 数据，为
 * false 表示同步 current 数据。
 * @param doc_start reported or desired 数据起始位置。
 * @param json_token json token 数组起始位置
 * @param tok_count 有效 json token 数量
 *
 * @return 
 */
int _tc_iot_sync_shadow_property(tc_iot_shadow_property_def properties[], bool reported, const char * doc_start, jsmntok_t * json_token, int tok_count) {
    int i,j;
    jsmntok_t  * key_tok = NULL;
    jsmntok_t  * val_tok = NULL;
    char field_buf[TC_IOT_DEVICE_NAME_LEN+1];
    int field_len = sizeof(field_buf);
    tc_iot_shadow_number new_number = 0;
    tc_iot_shadow_bool new_bool = 0;
    int  key_len = 0, val_len = 0;
    const char * key_start;
    const char * val_start;
    tc_iot_shadow_property_def * p_prop = NULL;
    void  * ptr = NULL;
    tc_iot_event_message event_msg;

    if (!properties) {
        LOG_ERROR("properties is null");
        return TC_IOT_NULL_POINTER;
    }

    if (!doc_start) {
        LOG_ERROR("doc_start is null");
        return TC_IOT_NULL_POINTER;
    }

    if (!json_token) {
        LOG_ERROR("json_token is null");
        return TC_IOT_NULL_POINTER;
    }

    if (!tok_count) {
        LOG_ERROR("tok_count is invalid");
        return TC_IOT_INVALID_PARAMETER;
    }

    memset(field_buf, 0, sizeof(field_buf));

    for (i = 0; i < tok_count/2; i++) {
        /* 位置 0 是object对象，所以要从位置 1 开始取数据*/
        /* 2*i+1 为 key 字段，2*i + 2 为 value 字段*/
        key_tok = &(json_token[2*i + 1]);
        key_start = doc_start + key_tok->start;
        key_len = key_tok->end - key_tok->start;

        val_tok = &(json_token[2*i + 2]);
        val_start = doc_start + val_tok->start;
        val_len = val_tok->end - val_tok->start;
        for(j = 0; j < TC_IOT_PROP_TOTAL; j++) {
            p_prop = &properties[j];
            if (!p_prop->ptr) {
                LOG_ERROR("No.%d property ptr invalid.", j);
                continue;
            }

            if (strncmp(p_prop->name, key_start, key_len) == 0)  {
                if (p_prop->type == TC_IOT_SHADOW_TYPE_STRING) {
                    if (val_len > p_prop->len) {
                        LOG_ERROR("%s[%.*s] to long, skip this settings", 
                                p_prop->name,val_len, val_start);
                        continue;
                    }
                    strncpy(field_buf, val_start, val_len);
                    field_buf[val_len] = '\0';
                    if (reported) {
                        ptr = p_prop->reported_ptr;
                    } else {
                        ptr = p_prop->ptr;
                    }
                    if (strncmp(ptr, field_buf, val_len) != 0) {
                        LOG_TRACE("state change:[%s|%s -> %s]", p_prop->name, (const char *) ptr, field_buf);
                        strncpy(ptr, field_buf, val_len + 1);
                    }
                } else if (p_prop->type == TC_IOT_SHADOW_TYPE_BOOL) {
                    strncpy(field_buf, val_start, val_len);
                    field_buf[val_len] = '\0';
                    new_bool = (strncmp(TC_IOT_JSON_TRUE, field_buf, val_len) == 0);
                    if (reported) {
                        ptr = p_prop->reported_ptr;
                    } else {
                        ptr = p_prop->ptr;
                    }
                    if (new_bool != (*(tc_iot_shadow_bool *)ptr)) {
                        LOG_TRACE("state change:[%s|%s -> %s]", p_prop->name, (*(tc_iot_shadow_bool *) ptr)?"true":"false", field_buf);
                        *(tc_iot_shadow_bool *)ptr = new_bool;
                    }
                } else if (p_prop->type == TC_IOT_SHADOW_TYPE_NUMBER) {
                    strncpy(field_buf, val_start, val_len);
                    field_buf[val_len] = '\0';
                    new_number = atoi(field_buf);
                    if (reported) {
                        ptr = p_prop->reported_ptr;
                    } else {
                        ptr = p_prop->ptr;
                    }
                    if (new_number != (*(tc_iot_shadow_number *)ptr)) {
                        LOG_TRACE("state change:[%s|%d -> %s]", p_prop->name, (*(tc_iot_shadow_number *) ptr), field_buf);
                        *(tc_iot_shadow_number *)ptr = new_number;
                    }
                } else {
                    LOG_ERROR("%s type=%d invalid.", p_prop->name, p_prop->type);
                    continue;
                }
                if (p_prop->fn_change_notify) {
                    if (reported) {
                        event_msg.event = TC_IOT_SHADOW_EVENT_REPORTED_STATUS_SYNC;
                    } else {
                        event_msg.event = TC_IOT_SHADOW_EVENT_SERVER_CONTROL;
                    }
                    event_msg.data = p_prop;
                    p_prop->fn_change_notify(&event_msg, "tc_iot_sync_shadow_property", NULL);
                }
            }
        }
    }
    return 0;
}



/**
 * @brief _device_on_message_received 数据回调，处理 shadow_get 获取最新状态，或
 * 者影子服务推送的最新控制指令数据。
 *
 * @param md 影子数据消息
 */
void _device_on_message_received(tc_iot_message_data* md) {
    jsmntok_t  json_token[TC_IOT_MAX_JSON_TOKEN_COUNT];
    char field_buf[TC_IOT_DEVICE_NAME_LEN+1];
    int field_index = 0;
    const char * reported_start = NULL;
    int reported_len = 0;
    const char * desired_start = NULL;
    int desired_len = 0;
    int ret = 0;

    memset(field_buf, 0, sizeof(field_buf));

    tc_iot_mqtt_message* message = md->message;
    LOG_TRACE("[s->c] %.*s", (int)message->payloadlen, (char*)message->payload);

    /* 有效性检查 */
    ret = tc_iot_json_parse(message->payload, message->payloadlen, json_token, TC_IOT_ARRAY_LENGTH(json_token));
    if (ret <= 0) {
        return ;
    }

    field_index = tc_iot_json_find_token((char*)message->payload, json_token, ret, "method", field_buf, sizeof(field_buf));
    if (field_index <= 0 ) {
        LOG_ERROR("field method not found in JSON: %.*s", (int)message->payloadlen, (char*)message->payload);
        return ;
    }

    if (strncmp("control", field_buf, strlen(field_buf)) == 0) {
        LOG_TRACE("Control data receved.");
    } else if (strncmp("reply", field_buf, strlen(field_buf)) == 0) {
        LOG_TRACE("Reply pack recevied.");
    }

    /* 检查 reported 字段是否存在 */
    field_index = tc_iot_json_find_token((char*)message->payload, json_token, ret, 
            "payload.state.reported", NULL, 0);
    if (field_index <= 0 ) {
        /* LOG_TRACE("payload.state.reported not found"); */
    } else {
        reported_start = message->payload + json_token[field_index].start;
        reported_len = json_token[field_index].end - json_token[field_index].start;
        LOG_TRACE("payload.state.reported found:%.*s", reported_len, reported_start);
    }

    /* 检查 desired 字段是否存在 */
    field_index = tc_iot_json_find_token((char*)message->payload, json_token, ret, 
            "payload.state.desired", NULL, 0);
    if (field_index <= 0 ) {
        /* LOG_TRACE("payload.state.desired not found"); */
    } else {
        desired_start = message->payload + json_token[field_index].start;
        desired_len = json_token[field_index].end - json_token[field_index].start;
        LOG_TRACE("payload.state.desired found:%.*s", desired_len, desired_start);
    }

    /* 如果设备无本地存储，则设备重启后可先同步之前上
     * 报的状态。
     *
     * 如果设备有本地存储，一般情况下，重启后本地状态还会
     * 和服务端一致。不一致时，以本地设备状态优先，还是
     * 以服务端优先，可根据实际业务情况进行区别处理。
     * */
    if (reported_start) {
        ret = tc_iot_json_parse(reported_start,reported_len, json_token, TC_IOT_ARRAY_LENGTH(json_token));
        if (ret <= 0) {
            return ;
        }
        LOG_TRACE("---synchronizing reported status---");
        _tc_iot_sync_shadow_property(g_device_property_defs, true, reported_start, json_token, ret);
        LOG_TRACE("---synchronizing reported status success---");
    }

    /* 根据控制台或者 APP 端的指令，设定设备状态 */
    if (desired_start) {
        ret = tc_iot_json_parse(desired_start,desired_len, json_token, TC_IOT_ARRAY_LENGTH(json_token));
        if (ret <= 0) {
            return ;
        }
        LOG_TRACE("---synchronizing desired status---");
        _tc_iot_sync_shadow_property(g_device_property_defs, false, desired_start, json_token, ret);
        LOG_TRACE("---synchronizing desired status success---");
    }

    /* 如果状态发生变化，则更新设备状态 */
    if (reported_start || desired_start) {
        /* 上报最新的设备状态，并通知服务端去掉 
         * desired 数据，避免指令重复下发。 
         * */
        if (desired_start) {
            tc_iot_shadow_sync_to_server(&client, g_device_property_defs, TC_IOT_JSON_NULL);
        } else {
            tc_iot_shadow_sync_to_server(&client, g_device_property_defs, NULL);
        }
    } else {
        /* 服务端无 reported 和 desired 状态，
         * 说明设备首次启动或数据被重置，重新上报初始状态 */
        tc_iot_shadow_sync_to_server(&client, g_device_property_defs, NULL);
    }
}

bool tc_iot_device_sync_reported(tc_iot_shadow_property_def properties[]) {
    int i = 0;
    bool reported_changed = false;
    tc_iot_shadow_property_def * current = NULL;
    
    // 对比差异，并同步数据
    for (i = 0; i < TC_IOT_PROP_TOTAL; i++ ) {
        current = &properties[i];
        if (current->type == TC_IOT_SHADOW_TYPE_STRING) {
            if (strncmp(current->ptr, current->reported_ptr, current->len) != 0) {
                LOG_TRACE("%s changed", current->name);
                reported_changed = true;
                memcpy(current->reported_ptr, current->ptr, current->len);
            }
        } else if (current->type == TC_IOT_SHADOW_TYPE_NUMBER) {
            if (memcmp(current->ptr, current->reported_ptr, current->len) != 0) {
                LOG_TRACE("%s changed", current->name);
                reported_changed = true;
                memcpy(current->reported_ptr, current->ptr, current->len);
            }
        } else if (current->type == TC_IOT_SHADOW_TYPE_BOOL) {
            if (memcmp(current->ptr, current->reported_ptr, current->len) != 0) {
                LOG_TRACE("%s changed", current->name);
                reported_changed = true;
                memcpy(current->reported_ptr, current->ptr, current->len);
            }
        } else {
            LOG_ERROR("%s type=%d unkown.", current->name, current->type);
        }
    }

    return reported_changed;
}



/* 影子数据 Client  */
tc_iot_shadow_client client;

int run_shadow(tc_iot_shadow_config * p_client_config) {
    int timeout = 2000;
    int ret = 0;
    char buffer[512];
    int buffer_len = sizeof(buffer);
    tc_iot_shadow_client* p_shadow_client = &client;

    /* 初始化 shadow client */
    tc_iot_hal_printf("constructing mqtt shadow client.\n");
    ret = tc_iot_shadow_construct(p_shadow_client, p_client_config);
    if (ret != TC_IOT_SUCCESS) {
        tc_iot_hal_printf("construct shadow client failed, trouble shooting guide: " "%s#%d\n", TC_IOT_TROUBLE_SHOOTING_URL, ret);
        return 0;
    }

    tc_iot_hal_printf("construct mqtt shadow client success.\n");
    tc_iot_hal_printf("yield waiting for server push.\n");
    /* 执行 yield 收取影子服务端前序指令消息，清理历史状态。 */
    tc_iot_shadow_yield(p_shadow_client, timeout);
    tc_iot_hal_printf("yield waiting for server finished.\n");

    /* 通过get操作主动获取服务端影子设备状态，以便设备端同步更新至最新状态*/
    ret = tc_iot_shadow_get(p_shadow_client, buffer, buffer_len, get_message_ack_callback, TC_IOT_CONFIG_COMMAND_TIMEOUT_MS, NULL);
    LOG_TRACE("[c->s] shadow_get\n%s", buffer);

    /* 循环等待控制指令 */
    while (!stop) {
        tc_iot_shadow_yield(p_shadow_client, timeout);
    }

    tc_iot_hal_printf("Stopping\n");
    tc_iot_shadow_destroy(p_shadow_client);
    tc_iot_hal_printf("Exit success.\n");
    return 0;
}
