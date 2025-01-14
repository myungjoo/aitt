/*
 * Copyright (c) 2021-2022 Samsung Electronics Co., Ltd All Rights Reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "MQ.h"

#include <mqtt_protocol.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <stdexcept>

#include "log.h"

namespace aitt {

const std::string MQ::REPLY_SEQUENCE_NUM_KEY = "sequenceNum";
const std::string MQ::REPLY_IS_END_SEQUENCE_KEY = "isEndSequence";

MQ::MQ(const std::string &id, bool clear_session)
      : clear_session_(clear_session), mq_id(id), keep_alive(60), subscriber_iterator_updated(false)
{
    int ret = mosquitto_lib_init();
    if (ret != MOSQ_ERR_SUCCESS)
        throw std::runtime_error(mosquitto_strerror(ret));

    handle = mosquitto_new(mq_id.c_str(), clear_session_, this);
    if (!handle) {
        mosquitto_lib_cleanup();
        throw std::runtime_error("Failed to mosquitto_new");
    }

    ret = mosquitto_int_option(handle, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V5);
    if (ret != MOSQ_ERR_SUCCESS)
        throw std::runtime_error(mosquitto_strerror(ret));

    mosquitto_message_v5_callback_set(handle, MQTTMessageCallback);
}

void MQ::MQTTMessageCallback(mosquitto *handle, void *_mq, const mosquitto_message *msg,
      const mosquitto_property *props)
{
    RET_IF(_mq == nullptr);

    MQ *mq = static_cast<MQ *>(_mq);

    std::lock_guard<std::recursive_mutex> auto_lock(mq->subscribers_lock);

    mq->subscriber_iterator = mq->subscribers.begin();
    while (mq->subscriber_iterator != mq->subscribers.end()) {
        bool result = false;
        int ret = mosquitto_topic_matches_sub((*mq->subscriber_iterator)->topic.c_str(), msg->topic,
              &result);
        if (ret != MOSQ_ERR_SUCCESS) {
            ERR("Topic comparator error: %s", mosquitto_strerror(ret));
            return;
        }

        if (result)
            mq->InvokeCallback(msg, props);

        if (!mq->subscriber_iterator_updated)
            mq->subscriber_iterator++;
        else
            mq->subscriber_iterator_updated = false;
    }
}

void MQ::InvokeCallback(const mosquitto_message *msg, const mosquitto_property *props)
{
    MSG mq_msg;
    mq_msg.SetTopic(msg->topic);
    if (props) {
        const mosquitto_property *prop;

        char *response_topic = nullptr;
        prop = mosquitto_property_read_string(props, MQTT_PROP_RESPONSE_TOPIC, &response_topic,
              false);
        if (prop) {
            mq_msg.SetResponseTopic(response_topic);
            free(response_topic);
        }

        void *correlation = nullptr;
        uint16_t correlation_size = 0;
        prop = mosquitto_property_read_binary(props, MQTT_PROP_CORRELATION_DATA, &correlation,
              &correlation_size, false);
        if (prop == nullptr || correlation == nullptr)
            ERR("No Correlation Data");

        mq_msg.SetCorrelation(std::string((char *)correlation, correlation_size));
        if (correlation)
            free(correlation);

        char *name = nullptr;
        char *value = nullptr;
        prop = mosquitto_property_read_string_pair(props, MQTT_PROP_USER_PROPERTY, &name, &value,
              false);
        while (prop) {
            if (REPLY_SEQUENCE_NUM_KEY == name) {
                mq_msg.SetSequence(std::stoi(value));
            } else if (REPLY_IS_END_SEQUENCE_KEY == name) {
                mq_msg.SetEndSequence(std::stoi(value) == 1);
            } else {
                ERR("Unsupported property(%s, %s)", name, value);
            }
            free(name);
            free(value);

            prop = mosquitto_property_read_string_pair(prop, MQTT_PROP_USER_PROPERTY, &name, &value,
                  true);
        }
    }

    (*subscriber_iterator)
          ->cb(&mq_msg, msg->topic, msg->payload, msg->payloadlen, (*subscriber_iterator)->cbdata);
}

void MQ::Publish(const std::string &topic, const void *data, const size_t datalen, MQ::QoS qos,
      bool retain)
{
    int mid = -1;
    int ret = mosquitto_publish(handle, &mid, topic.c_str(), datalen, data, qos, retain);
    if (ret != MOSQ_ERR_SUCCESS)
        throw std::runtime_error(mosquitto_strerror(ret));

    // TODO:
    // Use the mId to verify the broker gets our publish message successfully
}

void MQ::PublishWithReply(const std::string &topic, const void *data, const size_t datalen,
      MQ::QoS qos, bool retain, const std::string &reply_topic, const std::string &correlation)
{
    int ret;
    int mid = -1;
    mosquitto_property *props = nullptr;

    ret = mosquitto_property_add_string(&props, MQTT_PROP_RESPONSE_TOPIC, reply_topic.c_str());
    if (ret != MOSQ_ERR_SUCCESS) {
        ERR("mosquitto_property_add_string(response-topic) Fail(%d)", ret);
        throw std::runtime_error(mosquitto_strerror(ret));
    }

    ret = mosquitto_property_add_binary(&props, MQTT_PROP_CORRELATION_DATA, correlation.c_str(),
          correlation.size());
    if (ret != MOSQ_ERR_SUCCESS) {
        ERR("mosquitto_property_add_binary(correlation) Fail(%d)", ret);
        throw std::runtime_error(mosquitto_strerror(ret));
    }
    ret = mosquitto_publish_v5(handle, &mid, topic.c_str(), datalen, data, qos, retain, props);
    if (ret != MOSQ_ERR_SUCCESS)
        throw std::runtime_error(mosquitto_strerror(ret));
}

void MQ::SendReply(MSG *msg, const void *data, const size_t datalen, MQ::QoS qos, bool retain)
{
    RET_IF(msg == nullptr);

    int ret;
    int mId = -1;
    mosquitto_property *props = nullptr;

    ret = mosquitto_property_add_binary(&props, MQTT_PROP_CORRELATION_DATA,
          msg->GetCorrelation().c_str(), msg->GetCorrelation().size());
    if (ret != MOSQ_ERR_SUCCESS) {
        ERR("mosquitto_property_add_binary(correlation) Fail(%d)", ret);
        throw std::runtime_error(mosquitto_strerror(ret));
    }

    ret = mosquitto_property_add_string_pair(&props, MQTT_PROP_USER_PROPERTY,
          REPLY_SEQUENCE_NUM_KEY.c_str(), std::to_string(msg->GetSequence()).c_str());
    if (ret != MOSQ_ERR_SUCCESS) {
        ERR("mosquitto_property_add_string_pair(squence number) Fail(%d)", ret);
        throw std::runtime_error(mosquitto_strerror(ret));
    }

    ret = mosquitto_property_add_string_pair(&props, MQTT_PROP_USER_PROPERTY,
          REPLY_IS_END_SEQUENCE_KEY.c_str(), std::to_string(msg->IsEndSequence()).c_str());
    if (ret != MOSQ_ERR_SUCCESS) {
        ERR("mosquitto_property_add_string_pair(is end sequence) Fail(%d)", ret);
        throw std::runtime_error(mosquitto_strerror(ret));
    }

    ret = mosquitto_publish_v5(handle, &mId, msg->GetResponseTopic().c_str(), datalen, data, qos,
          retain, props);
    if (ret != MOSQ_ERR_SUCCESS)
        throw std::runtime_error(mosquitto_strerror(ret));
}

void *MQ::Subscribe(const std::string &topic, const SubscribeCallback &cb, void *cbdata,
      MQ::QoS qos)
{
    std::lock_guard<std::recursive_mutex> auto_lock(subscribers_lock);
    SubscribeData *data = new SubscribeData(topic, cb, cbdata);
    subscribers.push_back(data);

    // TODO:
    // 와일드카드로 토픽을 여러번 subscribe 한 경우,
    // MessageCallback 이 subscribe 한 횟수만큼 불릴까?
    // 아니면, 한번만 불릴까?

    int mid = -1;
    int ret = mosquitto_subscribe(handle, &mid, topic.c_str(), qos);
    if (ret != MOSQ_ERR_SUCCESS)
        throw std::runtime_error(mosquitto_strerror(ret));

    DBG("Subscribe request is sent for %s, %p", topic.c_str(), data);
    // TODO:
    // Use the mId to verify the broker gets our unsubsribe request successfully
    return static_cast<void *>(data);
}

void *MQ::Unsubscribe(void *sub_handle)
{
    DBG("Get a lock");
    std::lock_guard<std::recursive_mutex> auto_lock(subscribers_lock);
    DBG("Got a lock");
    auto it = std::find(subscribers.begin(), subscribers.end(),
          static_cast<SubscribeData *>(sub_handle));

    if (it == subscribers.end()) {
        DBG("Element is not found: %p", reinterpret_cast<SubscribeData *>(sub_handle));
        throw std::runtime_error("Element is not found");
    }

    SubscribeData *data = static_cast<SubscribeData *>(sub_handle);

    if (subscriber_iterator == it) {
        subscriber_iterator = subscribers.erase(it);
        subscriber_iterator_updated = true;
    } else {
        subscribers.erase(it);
    }

    void *cbdata = data->cbdata;
    std::string topic = data->topic;
    delete data;

    int mid = -1;
    int ret = mosquitto_unsubscribe(handle, &mid, topic.c_str());
    if (ret != MOSQ_ERR_SUCCESS)
        throw std::runtime_error(mosquitto_strerror(ret));

    DBG("Unsubscribe request is sent for %s", topic.c_str());
    // TODO:
    // Use the mId to verify the broker gets our publish message successfully
    return cbdata;
}

void MQ::Connect(const std::string &host, int port, const std::string &topic, const void *msg,
      size_t szmsg, MQ::QoS qos, bool retain)
{
    int ret;

    if (!topic.empty()) {
        ret = mosquitto_will_set(handle, topic.c_str(), szmsg, msg, qos, retain);
        if (ret != MOSQ_ERR_SUCCESS)
            throw std::runtime_error(mosquitto_strerror(ret));
    }

    ret = mosquitto_connect(handle, host.c_str(), port, keep_alive);
    if (ret != MOSQ_ERR_SUCCESS)
        throw std::runtime_error(mosquitto_strerror(ret));
}

void MQ::Disconnect(void)
{
    int ret = mosquitto_disconnect(handle);
    if (ret != MOSQ_ERR_SUCCESS)
        throw std::runtime_error(mosquitto_strerror(ret));

    mosquitto_will_clear(handle);
}

void MQ::Start(void)
{
    int ret = mosquitto_loop_start(handle);
    if (ret != MOSQ_ERR_SUCCESS)
        throw std::runtime_error(mosquitto_strerror(ret));
}

void MQ::Stop(bool force)
{
    int ret = mosquitto_loop_stop(handle, force);
    if (ret != MOSQ_ERR_SUCCESS)
        throw std::runtime_error(mosquitto_strerror(ret));
}

bool MQ::CompareTopic(const std::string &left, const std::string &right)
{
    bool result = false;
    int ret = mosquitto_topic_matches_sub(left.c_str(), right.c_str(), &result);
    if (ret != MOSQ_ERR_SUCCESS)
        throw std::runtime_error(mosquitto_strerror(ret));
    return result;
}

MQ::~MQ(void)
{
    mosquitto_destroy(handle);

    int ret = mosquitto_lib_cleanup();

    // Fallthrough to the end of a program
    if (ret != MOSQ_ERR_SUCCESS)
        ERR("Failed to cleanup the mqtt library (%s)", mosquitto_strerror(ret));
}

inline MQ::SubscribeData::SubscribeData(const std::string topic, const SubscribeCallback &cb,
      void *cbdata)
      : topic(topic), cb(cb), cbdata(cbdata)
{
}

}  // namespace aitt
