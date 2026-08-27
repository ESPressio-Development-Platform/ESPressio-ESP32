#pragma once

#include "ESPressio_UDPEventTransport.hpp"
#include "ESPressio_TCPClientEventTransport.hpp"
#include "ESPressio_TCPServerEventTransport.hpp"
#include "ESPressio_TLSEventTransport.hpp"

#if __has_include(<WebSocketsClient.h>) && __has_include(<WebSocketsServer.h>)
#include "ESPressio_WebSocketClientEventTransport.hpp"
#include "ESPressio_WebSocketServerEventTransport.hpp"
#endif

#if __has_include(<PubSubClient.h>)
#include "ESPressio_MQTTEventTransport.hpp"
#endif
