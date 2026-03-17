// license:GPLv3+
// vpc-score-plugin — VPX plugin that reads game state from NVRAM and
// broadcasts events via a local WebSocket for vpc-score-agent to consume.

#include "plugins/MsgPlugin.h"
#include "plugins/LoggingPlugin.h"
#include "plugins/ControllerPlugin.h"
#include "plugins/VPXPlugin.h"

#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <cstdarg>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <algorithm>

// WebSocket / socket support
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#undef TEXT
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#include <windows.h>
#define SHUT_RDWR SD_BOTH
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <dlfcn.h>
#include <limits.h>
typedef int SOCKET;
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

#ifndef _WIN32
#define sprintf_s snprintf
#endif

namespace VpcScorePlugin
{

   ///////////////////////////////////////////////////////////////////////////////
   // Plugin API pointers
   ///////////////////////////////////////////////////////////////////////////////

   const MsgPluginAPI *msgApi = nullptr;
   uint32_t endpointId = 0;

   unsigned int getVpxApiId = 0;
   unsigned int onGameStartId = 0;
   unsigned int onGameEndId = 0;
   unsigned int onPrepareFrameId = 0;
   unsigned int vpxGameStartId = 0;
   unsigned int vpxGameEndId = 0;

   LPI_IMPLEMENT // logging support

#define LOGD LPI_LOGD
#define LOGI LPI_LOGI
#define LOGW LPI_LOGW
#define LOGE LPI_LOGE

       ///////////////////////////////////////////////////////////////////////////////
       // Game state
       ///////////////////////////////////////////////////////////////////////////////

       std::string currentRomName;
   std::string nvramMapsPath;
   std::string currentMapPath;
   std::vector<uint8_t> nvramData;
   size_t nvramBaseAddress = 0;
   bool nvramHighNibbleOnly = false;

   // Previous state for change detection
   std::vector<std::string> previousScores;
   int previousPlayerCount = 0;
   int previousCurrentPlayer = 0;
   int previousCurrentBall = 0;
   bool previousGameOver = false;
   bool gameEndSent = false;
   bool firstStateCheck = true;

   std::chrono::steady_clock::time_point gameStartTime;
   std::vector<std::string> lastGameEndScores;

   ///////////////////////////////////////////////////////////////////////////////
   // Timestamp helper
   ///////////////////////////////////////////////////////////////////////////////

   std::string getTimestamp()
   {
      auto now = std::chrono::system_clock::now();
      auto t = std::chrono::system_clock::to_time_t(now);
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
      std::stringstream ss;
      ss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%S");
      ss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
      return ss.str();
   }

   ///////////////////////////////////////////////////////////////////////////////
   // Minimal WebSocket server (loopback only, port 3123)
   ///////////////////////////////////////////////////////////////////////////////

   std::atomic<bool> wsServerRunning{false};
   std::thread wsServerThread;
   SOCKET wsServerSocket = INVALID_SOCKET;
   std::vector<SOCKET> wsClients;
   std::mutex wsClientsMutex;

   // Base64 encode for WebSocket handshake
   static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   std::string base64Encode(const unsigned char *data, size_t len)
   {
      std::string ret;
      int i = 0;
      unsigned char a3[3], a4[4];
      while (len--)
      {
         a3[i++] = *(data++);
         if (i == 3)
         {
            a4[0] = (a3[0] & 0xfc) >> 2;
            a4[1] = ((a3[0] & 0x03) << 4) + ((a3[1] & 0xf0) >> 4);
            a4[2] = ((a3[1] & 0x0f) << 2) + ((a3[2] & 0xc0) >> 6);
            a4[3] = a3[2] & 0x3f;
            for (i = 0; i < 4; i++)
               ret += b64chars[a4[i]];
            i = 0;
         }
      }
      if (i)
      {
         for (int j = i; j < 3; j++)
            a3[j] = '\0';
         a4[0] = (a3[0] & 0xfc) >> 2;
         a4[1] = ((a3[0] & 0x03) << 4) + ((a3[1] & 0xf0) >> 4);
         a4[2] = ((a3[1] & 0x0f) << 2) + ((a3[2] & 0xc0) >> 6);
         for (int j = 0; j < i + 1; j++)
            ret += b64chars[a4[j]];
         while (i++ < 3)
            ret += '=';
      }
      return ret;
   }

   // SHA-1 for WebSocket handshake
   void sha1(const std::string &input, unsigned char output[20])
   {
      uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
      std::vector<uint8_t> msg(input.begin(), input.end());
      size_t ml = msg.size() * 8;
      msg.push_back(0x80);
      while ((msg.size() % 64) != 56)
         msg.push_back(0);
      for (int i = 7; i >= 0; i--)
         msg.push_back((ml >> (i * 8)) & 0xFF);
      for (size_t chunk = 0; chunk < msg.size(); chunk += 64)
      {
         uint32_t w[80];
         for (int i = 0; i < 16; i++)
            w[i] = (msg[chunk + i * 4] << 24) | (msg[chunk + i * 4 + 1] << 16) | (msg[chunk + i * 4 + 2] << 8) | msg[chunk + i * 4 + 3];
         for (int i = 16; i < 80; i++)
         {
            uint32_t t = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (t << 1) | (t >> 31);
         }
         uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
         for (int i = 0; i < 80; i++)
         {
            uint32_t f, k;
            if (i < 20)
            {
               f = (b & c) | ((~b) & d);
               k = 0x5A827999;
            }
            else if (i < 40)
            {
               f = b ^ c ^ d;
               k = 0x6ED9EBA1;
            }
            else if (i < 60)
            {
               f = (b & c) | (b & d) | (c & d);
               k = 0x8F1BBCDC;
            }
            else
            {
               f = b ^ c ^ d;
               k = 0xCA62C1D6;
            }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = t;
         }
         h0 += a;
         h1 += b;
         h2 += c;
         h3 += d;
         h4 += e;
      }
      for (int i = 0; i < 4; i++)
      {
         output[i] = (h0 >> (24 - i * 8)) & 0xFF;
         output[i + 4] = (h1 >> (24 - i * 8)) & 0xFF;
         output[i + 8] = (h2 >> (24 - i * 8)) & 0xFF;
         output[i + 12] = (h3 >> (24 - i * 8)) & 0xFF;
         output[i + 16] = (h4 >> (24 - i * 8)) & 0xFF;
      }
   }

   // Send a single WebSocket text frame to a client
   void sendWsFrame(SOCKET sock, const std::string &message)
   {
      std::vector<uint8_t> frame;
      frame.push_back(0x81); // FIN + text opcode
      size_t len = message.size();
      if (len < 126)
      {
         frame.push_back((uint8_t)len);
      }
      else if (len < 65536)
      {
         frame.push_back(126);
         frame.push_back((len >> 8) & 0xFF);
         frame.push_back(len & 0xFF);
      }
      else
      {
         frame.push_back(127);
         for (int i = 7; i >= 0; i--)
            frame.push_back((len >> (i * 8)) & 0xFF);
      }
      frame.insert(frame.end(), message.begin(), message.end());
      send(sock, reinterpret_cast<const char *>(frame.data()), (int)frame.size(), 0);
   }

   // Broadcast a JSON message to all connected WebSocket clients
   void broadcastWebSocket(const std::string &message)
   {
      std::lock_guard<std::mutex> lock(wsClientsMutex);
      if (wsClients.empty())
         return;

      // Build frame once
      std::vector<uint8_t> frame;
      frame.push_back(0x81);
      size_t len = message.size();
      if (len < 126)
      {
         frame.push_back((uint8_t)len);
      }
      else if (len < 65536)
      {
         frame.push_back(126);
         frame.push_back((len >> 8) & 0xFF);
         frame.push_back(len & 0xFF);
      }
      else
      {
         frame.push_back(127);
         for (int i = 7; i >= 0; i--)
            frame.push_back((len >> (i * 8)) & 0xFF);
      }
      frame.insert(frame.end(), message.begin(), message.end());

      std::vector<SOCKET> dead;
      for (SOCKET client : wsClients)
      {
         if (client == INVALID_SOCKET)
         {
            dead.push_back(client);
            continue;
         }
         int flags = 0;
#ifndef _WIN32
         flags = MSG_NOSIGNAL;
#endif
         int result = send(client, reinterpret_cast<const char *>(frame.data()), (int)frame.size(), flags);
         if (result == SOCKET_ERROR || result <= 0)
            dead.push_back(client);
      }

      for (SOCKET s : dead)
      {
         auto it = std::find(wsClients.begin(), wsClients.end(), s);
         if (it != wsClients.end())
         {
            closesocket(*it);
            wsClients.erase(it);
         }
      }
      if (!dead.empty())
         LOGI("Removed %zu disconnected client(s), %zu remain", dead.size(), wsClients.size());
   }

   // WebSocket server thread — accepts connections and completes handshake
   void webSocketServerThread()
   {
      LOGI("WebSocket server thread starting on 127.0.0.1:3123");

      wsServerSocket = socket(AF_INET, SOCK_STREAM, 0);
      if (wsServerSocket == INVALID_SOCKET)
      {
         LOGE("Failed to create server socket");
         return;
      }

      int opt = 1;
      setsockopt(wsServerSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));
#ifndef _WIN32
      setsockopt(wsServerSocket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
      setsockopt(wsServerSocket, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char *>(&opt), sizeof(opt));

      linger lin{};
      lin.l_onoff = 1;
      lin.l_linger = 0;
      setsockopt(wsServerSocket, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *>(&lin), sizeof(lin));

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // loopback only — not exposed to network
      addr.sin_port = htons(3123);

      if (bind(wsServerSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR)
      {
         LOGE("Failed to bind to 127.0.0.1:3123");
         closesocket(wsServerSocket);
         wsServerSocket = INVALID_SOCKET;
         return;
      }
      if (listen(wsServerSocket, 5) == SOCKET_ERROR)
      {
         LOGE("Failed to listen");
         closesocket(wsServerSocket);
         wsServerSocket = INVALID_SOCKET;
         return;
      }

      LOGI("WebSocket server listening on 127.0.0.1:3123 (loopback only)");

      while (wsServerRunning)
      {
         fd_set readfds;
         FD_ZERO(&readfds);
         FD_SET(wsServerSocket, &readfds);
         timeval timeout{};
         timeout.tv_sec = 0;
         timeout.tv_usec = 100000;
         int activity = select((int)wsServerSocket + 1, &readfds, nullptr, nullptr, &timeout);
         if (activity < 0 || !wsServerRunning)
            break;
         if (activity == 0)
            continue;
         if (!FD_ISSET(wsServerSocket, &readfds))
            continue;

         sockaddr_in clientAddr{};
         socklen_t clientLen = sizeof(clientAddr);
         SOCKET clientSocket = accept(wsServerSocket, reinterpret_cast<sockaddr *>(&clientAddr), &clientLen);
         if (clientSocket == INVALID_SOCKET)
            continue;

         LOGI("New connection from %s", inet_ntoa(clientAddr.sin_addr));

         // Set TCP_NODELAY and receive timeout for handshake
         int nodelay = 1;
         setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
         timeval recvTimeout{};
         recvTimeout.tv_sec = 5;
         setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&recvTimeout), sizeof(recvTimeout));

         // Read HTTP upgrade request
         char buf[4096] = {0};
         int total = 0, n = 0;
         do
         {
            n = recv(clientSocket, buf + total, sizeof(buf) - total - 1, 0);
            if (n > 0)
            {
               total += n;
               buf[total] = '\0';
               if (strstr(buf, "\r\n\r\n"))
                  break;
            }
            else
               break;
         } while (total < (int)sizeof(buf) - 1);

         bool ok = false;
         if (total > 0)
         {
            std::string req(buf);
            size_t kp = req.find("Sec-WebSocket-Key: ");
            if (kp != std::string::npos)
            {
               kp += 19;
               size_t ke = req.find("\r\n", kp);
               if (ke != std::string::npos)
               {
                  std::string key = req.substr(kp, ke - kp);
                  key += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
                  unsigned char hash[20];
                  sha1(key, hash);
                  std::string accept = base64Encode(hash, 20);
                  std::string resp =
                      "HTTP/1.1 101 Switching Protocols\r\n"
                      "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: " +
                      accept + "\r\n\r\n";
                  if (send(clientSocket, resp.c_str(), (int)resp.length(), 0) > 0)
                  {
                     {
                        std::lock_guard<std::mutex> lock(wsClientsMutex);
                        wsClients.push_back(clientSocket);
                     }
                     LOGI("WebSocket handshake complete, %zu client(s) connected", wsClients.size());
                     // Send connected event
                     std::stringstream cm;
                     cm << "{\"type\":\"connected\",\"timestamp\":\"" << getTimestamp()
                        << "\",\"server\":\"vpc-score-plugin\",\"version\":\"1.0\""
                        << (currentRomName.empty() ? "" : ",\"rom\":\"" + currentRomName + "\"")
                        << "}";
                     sendWsFrame(clientSocket, cm.str());
                     ok = true;
                  }
               }
            }
         }
         if (!ok)
         {
            LOGW("WebSocket handshake failed — closing connection");
            closesocket(clientSocket);
         }
      }

      LOGI("WebSocket server thread exiting");
   }

   ///////////////////////////////////////////////////////////////////////////////
   // Get plugin directory
   ///////////////////////////////////////////////////////////////////////////////

   std::string getPluginDirectory()
   {
#ifdef _WIN32
      HMODULE hm = nullptr;
      if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR) "VpcScorePluginLoad", &hm))
         return "";
      char path[MAX_PATH];
      if (!GetModuleFileNameA(hm, path, MAX_PATH))
         return "";
      std::string s(path);
      size_t p = s.find_last_of("\\/");
      return (p != std::string::npos) ? s.substr(0, p) : "";
#else
      Dl_info info{};
      if (!dladdr((void *)&getPluginDirectory, &info) || !info.dli_fname)
         return "";
      char real[PATH_MAX];
      if (!realpath(info.dli_fname, real))
         return "";
      std::string s(real);
      size_t p = s.find_last_of('/');
      return (p != std::string::npos) ? s.substr(0, p) : "";
#endif
   }

   ///////////////////////////////////////////////////////////////////////////////
   // Minimal JSON parser (verbatim from score-server — well tested)
   ///////////////////////////////////////////////////////////////////////////////

   struct JsonValue
   {
      enum Type
      {
         STRING,
         NUMBER,
         OBJECT,
         ARRAY,
         BOOLEAN,
         NULL_TYPE
      };
      Type type;
      std::string strValue;
      int64_t numValue = 0;
      std::map<std::string, JsonValue *> objValue;
      std::vector<JsonValue *> arrayValue;
      bool boolValue = false;

      JsonValue() : type(NULL_TYPE) {}
      ~JsonValue()
      {
         for (auto &p : objValue)
            delete p.second;
         for (auto &v : arrayValue)
            delete v;
      }

      const JsonValue *get(const char *key) const
      {
         auto it = objValue.find(key);
         return (it != objValue.end()) ? it->second : nullptr;
      }
      const JsonValue *at(size_t i) const { return (i < arrayValue.size()) ? arrayValue[i] : nullptr; }
   };

   class SimpleJsonParser
   {
      const char *pos;
      const char *end;
      void skipWS()
      {
         while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r'))
            pos++;
      }
      std::string parseString()
      {
         if (*pos != '"')
            return "";
         pos++;
         std::string r;
         while (pos < end && *pos != '"')
         {
            if (*pos == '\\' && pos + 1 < end)
            {
               pos++;
               switch (*pos)
               {
               case 'n':
                  r += '\n';
                  break;
               case 't':
                  r += '\t';
                  break;
               case 'r':
                  r += '\r';
                  break;
               case '\\':
                  r += '\\';
                  break;
               case '"':
                  r += '"';
                  break;
               case 'u':
                  pos += 4;
                  r += '?';
                  break;
               default:
                  r += *pos;
               }
            }
            else
               r += *pos;
            pos++;
         }
         if (pos < end)
            pos++;
         return r;
      }
      int64_t parseNumber()
      {
         int64_t r = 0;
         bool neg = false;
         if (*pos == '-')
         {
            neg = true;
            pos++;
         }
         while (pos < end && *pos >= '0' && *pos <= '9')
         {
            r = r * 10 + (*pos - '0');
            pos++;
         }
         if (pos < end && *pos == '.')
         {
            pos++;
            while (pos < end && *pos >= '0' && *pos <= '9')
               pos++;
         }
         return neg ? -r : r;
      }
      JsonValue *parseObject()
      {
         JsonValue *o = new JsonValue();
         o->type = JsonValue::OBJECT;
         pos++;
         skipWS();
         while (pos < end && *pos != '}')
         {
            skipWS();
            if (*pos == '}')
               break;
            if (*pos != '"')
            {
               delete o;
               return nullptr;
            }
            std::string k = parseString();
            skipWS();
            if (pos >= end || *pos != ':')
            {
               delete o;
               return nullptr;
            }
            pos++;
            skipWS();
            JsonValue *v = parseValue();
            if (!v)
            {
               delete o;
               return nullptr;
            }
            o->objValue[k] = v;
            skipWS();
            if (pos < end && *pos == ',')
            {
               pos++;
               skipWS();
            }
         }
         if (pos < end)
            pos++;
         return o;
      }
      JsonValue *parseArray()
      {
         JsonValue *a = new JsonValue();
         a->type = JsonValue::ARRAY;
         pos++;
         skipWS();
         while (pos < end && *pos != ']')
         {
            skipWS();
            if (*pos == ']')
               break;
            JsonValue *v = parseValue();
            if (!v)
            {
               delete a;
               return nullptr;
            }
            a->arrayValue.push_back(v);
            skipWS();
            if (pos < end && *pos == ',')
            {
               pos++;
               skipWS();
            }
         }
         if (pos < end)
            pos++;
         return a;
      }
      JsonValue *parseValue()
      {
         skipWS();
         if (pos >= end)
            return nullptr;
         if (*pos == '"')
         {
            JsonValue *v = new JsonValue();
            v->type = JsonValue::STRING;
            v->strValue = parseString();
            return v;
         }
         if (*pos == '{')
            return parseObject();
         if (*pos == '[')
            return parseArray();
         if (*pos == 't' || *pos == 'f')
         {
            JsonValue *v = new JsonValue();
            v->type = JsonValue::BOOLEAN;
            v->boolValue = (*pos == 't');
            pos += (*pos == 't' ? 4 : 5);
            return v;
         }
         if (*pos == 'n')
         {
            JsonValue *v = new JsonValue();
            v->type = JsonValue::NULL_TYPE;
            pos += 4;
            return v;
         }
         if (*pos == '-' || (*pos >= '0' && *pos <= '9'))
         {
            JsonValue *v = new JsonValue();
            v->type = JsonValue::NUMBER;
            v->numValue = parseNumber();
            return v;
         }
         return nullptr;
      }

   public:
      JsonValue *parse(const std::string &json)
      {
         pos = json.c_str();
         end = pos + json.size();
         skipWS();
         return parseValue();
      }
   };

   ///////////////////////////////////////////////////////////////////////////////
   // NVRAM reading & decoding
   ///////////////////////////////////////////////////////////////////////////////

   // Forward declaration
   size_t parseStartAddress(const JsonValue *startVal, bool adjustForBase = true);

   std::string decodeBCD(const std::vector<uint8_t> &nvram, size_t start, size_t length)
   {
      if (start + length > nvram.size())
         return "ERROR";
      std::stringstream r;
      if (nvramHighNibbleOnly)
      {
         for (int i = (int)length - 1; i >= 0; i--)
         {
            uint8_t h = (nvram[start + i] >> 4) & 0x0F;
            if (h <= 9)
               r << (char)('0' + h);
         }
      }
      else
      {
         for (size_t i = 0; i < length; i++)
         {
            uint8_t b = nvram[start + i];
            uint8_t h = (b >> 4) & 0x0F, l = b & 0x0F;
            if (h <= 9)
               r << (char)('0' + h);
            if (l <= 9)
               r << (char)('0' + l);
         }
      }
      return r.str();
   }

   std::string decodeBCDFromOffsets(const std::vector<uint8_t> &nvram, const JsonValue *offsetsArray)
   {
      if (!offsetsArray || offsetsArray->type != JsonValue::ARRAY)
         return "ERROR";
      std::stringstream r;
      for (size_t i = 0; i < offsetsArray->arrayValue.size(); i++)
      {
         size_t off = parseStartAddress(offsetsArray->at(i));
         if (off >= nvram.size())
            continue;
         uint8_t b = nvram[off];
         uint8_t h = (b >> 4) & 0x0F, l = b & 0x0F;
         if (h <= 9)
            r << (char)('0' + h);
         if (l <= 9)
            r << (char)('0' + l);
      }
      return r.str();
   }

   std::string applyScale(const std::string &score, int scale)
   {
      if (scale <= 1 || score.empty() || score == "ERROR" || score == "???")
         return score;
      int zeros = 0;
      int s = scale;
      while (s > 1 && s % 10 == 0)
      {
         zeros++;
         s /= 10;
      }
      if (zeros > 0 && s == 1)
         return score + std::string(zeros, '0');
      try
      {
         return std::to_string((unsigned long long)std::stoull(score) * scale);
      }
      catch (...)
      {
         return score;
      }
   }

   std::string decodeChar(const std::vector<uint8_t> &nvram, size_t start, size_t length, const std::string &charMap = "")
   {
      if (start + length > nvram.size())
         return "ERROR";
      std::string r;
      for (size_t i = 0; i < length; i++)
      {
         uint8_t b = nvram[start + i];
         if (!charMap.empty())
            r += (b < charMap.length() ? charMap[b] : '?');
         else
            r += ((b >= 32 && b <= 126) ? (char)b : '?');
      }
      return r;
   }

   size_t parseStartAddress(const JsonValue *startVal, bool adjustForBase)
   {
      if (!startVal)
         return 0;
      size_t addr = 0;
      if (startVal->type == JsonValue::STRING)
         addr = std::stoul(startVal->strValue, nullptr, 16);
      else if (startVal->type == JsonValue::NUMBER)
         addr = startVal->numValue;
      if (adjustForBase && addr >= nvramBaseAddress)
         addr -= nvramBaseAddress;
      return addr;
   }

   int decodeValue(const std::vector<uint8_t> &nvram, const JsonValue *fieldObj)
   {
      if (!fieldObj || fieldObj->type != JsonValue::OBJECT)
         return 0;
      const JsonValue *startVal = fieldObj->get("start");
      if (startVal)
      {
         size_t raw = parseStartAddress(startVal, false);
         if (raw < nvramBaseAddress)
            return 0;
      }
      size_t start = parseStartAddress(startVal);
      if (start >= nvram.size())
         return 0;
      const JsonValue *encVal = fieldObj->get("encoding");
      std::string enc = (encVal && encVal->type == JsonValue::STRING) ? encVal->strValue : "int";
      uint32_t mask = 0xFF;
      const JsonValue *maskVal = fieldObj->get("mask");
      if (maskVal)
      {
         if (maskVal->type == JsonValue::STRING)
            mask = std::stoul(maskVal->strValue, nullptr, 16);
         else if (maskVal->type == JsonValue::NUMBER)
            mask = (uint32_t)maskVal->numValue;
      }
      int offset = 0;
      const JsonValue *offVal = fieldObj->get("offset");
      if (offVal && offVal->type == JsonValue::NUMBER)
         offset = (int)offVal->numValue;
      int val = 0;
      if (enc == "bcd")
      {
         uint8_t b = nvram[start] & mask;
         uint8_t h = (b >> 4) & 0x0F, l = b & 0x0F;
         if (h <= 9 && l <= 9)
            val = h * 10 + l;
      }
      else if (enc == "int")
         val = nvram[start] & mask;
      else if (enc == "bool")
      {
         const JsonValue *inv = fieldObj->get("invert");
         bool invert = (inv && inv->type == JsonValue::BOOLEAN && inv->boolValue);
         val = (nvram[start] & mask) ? 1 : 0;
         if (invert)
            val = !val;
      }
      return val + offset;
   }

   ///////////////////////////////////////////////////////////////////////////////
   // NVRAM map loading — resolves map path and loads platform base address
   // Returns true and populates currentMapPath, nvramBaseAddress, nvramHighNibbleOnly
   ///////////////////////////////////////////////////////////////////////////////

   bool loadNvramMap(const std::string &romName)
   {
      std::string indexPath = nvramMapsPath + "/index.json";
      std::ifstream indexFile(indexPath);
      if (!indexFile.is_open())
      {
         LOGE("Cannot open index.json: %s", indexPath.c_str());
         return false;
      }
      std::stringstream buf;
      buf << indexFile.rdbuf();
      SimpleJsonParser parser;
      JsonValue *index = parser.parse(buf.str());
      if (!index || index->type != JsonValue::OBJECT)
      {
         delete index;
         LOGE("Failed to parse index.json");
         return false;
      }
      const JsonValue *mapPathVal = index->get(romName.c_str());
      if (!mapPathVal || mapPathVal->type != JsonValue::STRING)
      {
         LOGW("No nvram map for ROM: %s", romName.c_str());
         delete index;
         return false;
      }
      std::string mapPath = nvramMapsPath + "/" + mapPathVal->strValue;
      delete index;

      // Read map file to get platform
      std::ifstream mapFile(mapPath);
      if (!mapFile.is_open())
      {
         LOGE("Cannot open map: %s", mapPath.c_str());
         return false;
      }
      std::stringstream mapBuf;
      mapBuf << mapFile.rdbuf();
      JsonValue *mapRoot = parser.parse(mapBuf.str());
      if (!mapRoot || mapRoot->type != JsonValue::OBJECT)
      {
         delete mapRoot;
         LOGE("Failed to parse map: %s", mapPath.c_str());
         return false;
      }

      const JsonValue *meta = mapRoot->get("_metadata");
      if (!meta || meta->type != JsonValue::OBJECT)
      {
         delete mapRoot;
         LOGE("Missing _metadata in map");
         return false;
      }
      const JsonValue *platVal = meta->get("platform");
      if (!platVal || platVal->type != JsonValue::STRING)
      {
         delete mapRoot;
         LOGE("Missing platform in _metadata");
         return false;
      }
      std::string platform = platVal->strValue;
      delete mapRoot;

      // Load platform file
      std::string platPath = nvramMapsPath + "/platforms/" + platform + ".json";
      std::ifstream platFile(platPath);
      if (!platFile.is_open())
      {
         LOGE("Cannot open platform: %s", platPath.c_str());
         return false;
      }
      std::stringstream platBuf;
      platBuf << platFile.rdbuf();
      JsonValue *platRoot = parser.parse(platBuf.str());
      if (!platRoot || platRoot->type != JsonValue::OBJECT)
      {
         delete platRoot;
         LOGE("Failed to parse platform JSON");
         return false;
      }

      nvramBaseAddress = 0;
      nvramHighNibbleOnly = false;
      const JsonValue *memLayout = platRoot->get("memory_layout");
      if (memLayout && memLayout->type == JsonValue::ARRAY)
      {
         for (size_t i = 0; i < memLayout->arrayValue.size(); i++)
         {
            const JsonValue *entry = memLayout->at(i);
            if (!entry || entry->type != JsonValue::OBJECT)
               continue;
            const JsonValue *typeVal = entry->get("type");
            if (typeVal && typeVal->type == JsonValue::STRING && typeVal->strValue == "nvram")
            {
               const JsonValue *addrVal = entry->get("address");
               if (addrVal && addrVal->type == JsonValue::STRING)
                  nvramBaseAddress = parseStartAddress(addrVal, false);
               const JsonValue *nibVal = entry->get("nibble");
               if (nibVal && nibVal->type == JsonValue::STRING && nibVal->strValue == "high")
                  nvramHighNibbleOnly = true;
               LOGI("Platform %s: NVRAM base=0x%zx, highNibble=%d", platform.c_str(), nvramBaseAddress, (int)nvramHighNibbleOnly);
               break;
            }
         }
      }
      delete platRoot;

      currentMapPath = mapPath;
      LOGI("Loaded NVRAM map: %s", mapPath.c_str());
      return true;
   }

   ///////////////////////////////////////////////////////////////////////////////
   // Read NVRAM from disk (.nv file) — fallback path
   ///////////////////////////////////////////////////////////////////////////////

   bool readNvramFromDisk(const std::string &romName, std::vector<uint8_t> &nvram)
   {
      // VPX stores .nv files alongside the ROM — common paths on Windows
      std::vector<std::string> searchPaths = {
          std::string(std::getenv("APPDATA") ? std::getenv("APPDATA") : "") + "\\VPinMAME\\nvram\\" + romName + ".nv",
          "nvram\\" + romName + ".nv",
      };
      for (const auto &path : searchPaths)
      {
         std::ifstream f(path, std::ios::binary);
         if (!f.is_open())
            continue;
         nvram.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
         LOGI("Read %zu bytes from %s", nvram.size(), path.c_str());
         return true;
      }
      LOGW("No .nv file found for ROM: %s", romName.c_str());
      return false;
   }

   ///////////////////////////////////////////////////////////////////////////////
   // Score extraction from NVRAM
   ///////////////////////////////////////////////////////////////////////////////

   struct GameState
   {
      int playerCount = 0;
      int currentPlayer = 0;
      int currentBall = 0;
      bool gameOver = false;
      bool hasGameOverFlag = false;
      std::vector<std::string> scores;
   };

   // Extract a single score entry from NVRAM
   std::string extractScore(const std::vector<uint8_t> &nvram, const JsonValue *scoreEntry)
   {
      if (!scoreEntry || scoreEntry->type != JsonValue::OBJECT)
         return "";
      const JsonValue *encVal = scoreEntry->get("encoding");
      const JsonValue *offsetsVal = scoreEntry->get("offsets");
      const JsonValue *startVal = scoreEntry->get("start");
      const JsonValue *lengthVal = scoreEntry->get("length");
      const JsonValue *scaleVal = scoreEntry->get("scale");
      if (!encVal)
         return "";
      std::string score;
      if (offsetsVal && offsetsVal->type == JsonValue::ARRAY)
      {
         if (encVal->strValue == "bcd")
            score = decodeBCDFromOffsets(nvram, offsetsVal);
         else
            score = "???";
      }
      else if (startVal && lengthVal)
      {
         size_t addr = parseStartAddress(startVal);
         if (encVal->strValue == "bcd")
         {
            score = decodeBCD(nvram, addr, (size_t)lengthVal->numValue);
         }
         else if (encVal->strValue == "int")
         {
            uint64_t val = 0;
            size_t len = (size_t)lengthVal->numValue;
            if (addr + len <= nvram.size())
            {
               for (size_t j = 0; j < len; j++)
                  val = (val << 8) | nvram[addr + j];
               score = std::to_string(val);
            }
            else
               score = "ERROR";
         }
         else
            score = "???";
      }
      else
         return "";
      if (scaleVal && scaleVal->type == JsonValue::NUMBER && scaleVal->numValue > 1)
         score = applyScale(score, (int)scaleVal->numValue);
      return score;
   }

   GameState readGameState(const std::vector<uint8_t> &nvram, const std::string &mapPath)
   {
      GameState gs;
      std::ifstream file(mapPath);
      if (!file.is_open())
         return gs;
      std::stringstream buf;
      buf << file.rdbuf();
      SimpleJsonParser parser;
      JsonValue *root = parser.parse(buf.str());
      if (!root || root->type != JsonValue::OBJECT)
      {
         delete root;
         return gs;
      }
      const JsonValue *gameState = root->get("game_state");
      if (!gameState || gameState->type != JsonValue::OBJECT)
      {
         delete root;
         return gs;
      }

      gs.playerCount = decodeValue(nvram, gameState->get("player_count"));
      gs.currentPlayer = decodeValue(nvram, gameState->get("current_player"));
      gs.currentBall = decodeValue(nvram, gameState->get("current_ball"));

      const JsonValue *gameOverObj = gameState->get("game_over");
      gs.hasGameOverFlag = (gameOverObj != nullptr);
      if (gs.hasGameOverFlag)
         gs.gameOver = (decodeValue(nvram, gameOverObj) != 0);

      const JsonValue *scores = gameState->get("scores");
      if (scores && scores->type == JsonValue::ARRAY)
      {
         // If playerCount is 0 (RAM not accessible), infer from non-zero scores
         if (gs.playerCount == 0)
         {
            int inferred = 0;
            for (size_t i = 0; i < scores->arrayValue.size(); i++)
            {
               std::string s = extractScore(nvram, scores->at(i));
               if (!s.empty() && s != "0" && s != "ERROR" && s != "???")
               {
                  bool allZero = true;
                  for (char c : s)
                  {
                     if (c != '0')
                     {
                        allZero = false;
                        break;
                     }
                  }
                  if (!allZero)
                     inferred = (int)i + 1;
               }
            }
            gs.playerCount = (inferred > 0) ? inferred : 1;
         }
         for (size_t i = 0; i < scores->arrayValue.size() && i < (size_t)gs.playerCount; i++)
         {
            gs.scores.push_back(extractScore(nvram, scores->at(i)));
         }
      }

      delete root;
      return gs;
   }

   ///////////////////////////////////////////////////////////////////////////////
   // JSON event builders
   ///////////////////////////////////////////////////////////////////////////////

   std::string buildGameStartMsg()
   {
      std::stringstream ss;
      ss << "{\"type\":\"game_start\","
         << "\"timestamp\":\"" << getTimestamp() << "\","
         << "\"rom\":\"" << currentRomName << "\"}";
      return ss.str();
   }

   std::string buildCurrentScoresMsg(const GameState &gs)
   {
      std::stringstream ss;
      ss << "{\"type\":\"current_scores\","
         << "\"timestamp\":\"" << getTimestamp() << "\","
         << "\"rom\":\"" << currentRomName << "\","
         << "\"players\":" << gs.playerCount << ","
         << "\"current_player\":" << gs.currentPlayer << ","
         << "\"current_ball\":" << gs.currentBall << ","
         << "\"scores\":[";
      for (size_t i = 0; i < gs.scores.size(); i++)
      {
         if (i > 0)
            ss << ",";
         ss << "{\"player\":\"Player " << (i + 1) << "\",\"score\":\"" << gs.scores[i] << "\"}";
      }
      ss << "]}";
      return ss.str();
   }

   std::string buildGameEndMsg(const GameState &gs, const std::string &reason)
   {
      auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - gameStartTime)
                          .count();
      std::stringstream ss;
      ss << "{\"type\":\"game_end\","
         << "\"timestamp\":\"" << getTimestamp() << "\","
         << "\"rom\":\"" << currentRomName << "\","
         << "\"reason\":\"" << reason << "\","
         << "\"game_duration\":" << duration << ","
         << "\"players\":" << gs.playerCount << ","
         << "\"scores\":[";
      for (size_t i = 0; i < gs.scores.size(); i++)
      {
         if (i > 0)
            ss << ",";
         ss << "{\"player\":\"Player " << (i + 1) << "\",\"score\":\"" << gs.scores[i] << "\"}";
      }
      ss << "]}";
      return ss.str();
   }

   ///////////////////////////////////////////////////////////////////////////////
   // VPX event handlers
   ///////////////////////////////////////////////////////////////////////////////

   void onGameStart(const unsigned int eventId, void * /*userData*/, void *eventData)
   {
      LOGI("onGameStart fired — eventId=%u eventData=%p", eventId, eventData);
      CtlOnGameStartMsg *msg = static_cast<CtlOnGameStartMsg *>(eventData);
      if (!msg || !msg->gameId)
      {
         LOGW("onGameStart: null msg or gameId");
         return;
      }
      currentRomName = msg->gameId;
      gameStartTime = std::chrono::steady_clock::now();
      previousScores.clear();
      previousPlayerCount = 0;
      previousCurrentPlayer = 0;
      previousCurrentBall = 0;
      previousGameOver = false;
      gameEndSent = false;
      firstStateCheck = true;
      lastGameEndScores.clear();
      nvramData.clear();

      LOGI("Game started: %s", currentRomName.c_str());

      // Load NVRAM map
      if (!loadNvramMap(currentRomName))
      {
         LOGW("No NVRAM map for %s — current_scores will not be emitted", currentRomName.c_str());
      }

      broadcastWebSocket(buildGameStartMsg());
   }

   void onGameEnd(const unsigned int /*eventId*/, void * /*userData*/, void * /*eventData*/)
   {
      auto duration = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - gameStartTime);
      LOGI("Game ended: %s (duration: %llds)", currentRomName.c_str(), (long long)duration.count());

      if (!gameEndSent)
      {
         // Read NVRAM one final time for the end payload
         GameState gs;
         if (!currentMapPath.empty() && readNvramFromDisk(currentRomName, nvramData))
            gs = readGameState(nvramData, currentMapPath);
         else
            gs.scores = lastGameEndScores;
         broadcastWebSocket(buildGameEndMsg(gs, "game_over"));
         gameEndSent = true;
      }

      currentMapPath.clear();
      currentRomName.clear();
   }

   void onPrepareFrame(const unsigned int /*eventId*/, void * /*userData*/, void * /*eventData*/)
   {
      static int frameCount = 0;
      if (++frameCount % 300 == 1)
         LOGI("onPrepareFrame firing — frame=%d rom=%s mapPath=%s", frameCount, currentRomName.c_str(), currentMapPath.c_str());
      if (currentMapPath.empty())
         return;

      // Read .nv file from disk
      if (!readNvramFromDisk(currentRomName, nvramData))
         return;

      GameState gs = readGameState(nvramData, currentMapPath);

      // Game-over detection
      auto hasNonZero = [](const std::vector<std::string> &s)
      {
         for (auto &sc : s)
         {
            if (sc.empty() || sc == "ERROR" || sc == "???")
               continue;
            for (char c : sc)
               if (c != '0')
                  return true;
         }
         return false;
      };

      if (!firstStateCheck && !gameEndSent)
      {
         bool triggerEnd = false;
         if (gs.hasGameOverFlag && gs.gameOver && !previousGameOver)
         {
            LOGI("Game over detected via NVRAM flag");
            triggerEnd = true;
         }
         else if (gs.currentBall == 0 && previousCurrentBall > 0)
         {
            if (hasNonZero(gs.scores) && gs.scores != lastGameEndScores)
            {
               LOGI("Game over detected via ball-drop fallback");
               triggerEnd = true;
            }
         }
         if (triggerEnd)
         {
            broadcastWebSocket(buildGameEndMsg(gs, "game_over"));
            gameEndSent = true;
            lastGameEndScores = gs.scores;
         }
      }

      // Re-arm on new game
      if (gameEndSent && gs.currentBall > 0 && previousCurrentBall == 0)
      {
         LOGI("New game detected — re-arming game_end trigger");
         gameEndSent = false;
      }

      if (firstStateCheck)
      {
         previousGameOver = gs.gameOver;
         if (gs.currentBall == 0 && hasNonZero(gs.scores))
            lastGameEndScores = gs.scores;
         firstStateCheck = false;
      }

      // Broadcast only if state changed
      bool changed = (gs.playerCount != previousPlayerCount ||
                      gs.currentPlayer != previousCurrentPlayer ||
                      gs.currentBall != previousCurrentBall ||
                      gs.scores != previousScores ||
                      gs.gameOver != previousGameOver);

      if (changed)
      {
         previousPlayerCount = gs.playerCount;
         previousCurrentPlayer = gs.currentPlayer;
         previousCurrentBall = gs.currentBall;
         previousScores = gs.scores;
         previousGameOver = gs.gameOver;
         broadcastWebSocket(buildCurrentScoresMsg(gs));
      }
   }

} // namespace VpcScorePlugin

///////////////////////////////////////////////////////////////////////////////
// Plugin entry points
///////////////////////////////////////////////////////////////////////////////

using namespace VpcScorePlugin;

MSGPI_EXPORT void MSGPIAPI VpcScorePluginLoad(const uint32_t sessionId, const MsgPluginAPI *api)
{
   msgApi = api;
   endpointId = sessionId;

   LPISetup(endpointId, msgApi);
   LOGI("VPC Score Plugin loading");

   std::string pluginDir = getPluginDirectory();
   if (pluginDir.empty())
   {
      LOGE("Failed to determine plugin directory");
      return;
   }

   nvramMapsPath = pluginDir + "/nvram-maps";
   struct stat st;
   if (stat(nvramMapsPath.c_str(), &st) != 0 || !(st.st_mode & S_IFDIR))
   {
      LOGE("nvram-maps directory not found at: %s", nvramMapsPath.c_str());
      return;
   }
   LOGI("NVRAM maps path: %s", nvramMapsPath.c_str());

#ifdef _WIN32
   WSADATA wsaData;
   if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
   {
      LOGE("WSAStartup failed");
      return;
   }
#endif

   // Subscribe to game lifecycle events via Controller namespace (PinMAME)
   msgApi->SubscribeMsg(endpointId, onGameStartId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_START), onGameStart, nullptr);
   msgApi->SubscribeMsg(endpointId, onGameEndId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_END), onGameEnd, nullptr);
   // Also subscribe via VPX namespace in case the event fires there instead
   msgApi->SubscribeMsg(endpointId, vpxGameStartId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_START), onGameStart, nullptr);
   msgApi->SubscribeMsg(endpointId, vpxGameEndId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END), onGameEnd, nullptr);
   msgApi->SubscribeMsg(endpointId, onPrepareFrameId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME), onPrepareFrame, nullptr);
   LOGI("Subscribed to game events on both Controller and VPX namespaces");

   // Get VPX API (not currently used but good practice to hold)
   getVpxApiId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API);

   // Start WebSocket server
   wsServerRunning = true;
   wsServerThread = std::thread(webSocketServerThread);
   LOGI("VPC Score Plugin loaded — WebSocket server started on 127.0.0.1:3123");
}

MSGPI_EXPORT void MSGPIAPI VpcScorePluginUnload()
{
   LOGI("VPC Score Plugin unloading");

   // Stop WebSocket server
   wsServerRunning = false;
   {
      std::lock_guard<std::mutex> lock(wsClientsMutex);
      for (SOCKET c : wsClients)
      {
         shutdown(c, SHUT_RDWR);
         closesocket(c);
      }
      wsClients.clear();
   }
   if (wsServerSocket != INVALID_SOCKET)
   {
      shutdown(wsServerSocket, SHUT_RDWR);
      closesocket(wsServerSocket);
      wsServerSocket = INVALID_SOCKET;
   }
   if (wsServerThread.joinable())
      wsServerThread.join();
   LOGI("WebSocket server stopped");

#ifdef _WIN32
   WSACleanup();
#endif

   msgApi->UnsubscribeMsg(onGameStartId, onGameStart);
   msgApi->UnsubscribeMsg(onGameEndId, onGameEnd);
   msgApi->UnsubscribeMsg(vpxGameStartId, onGameStart);
   msgApi->UnsubscribeMsg(vpxGameEndId, onGameEnd);
   msgApi->UnsubscribeMsg(onPrepareFrameId, onPrepareFrame);
   msgApi->ReleaseMsgID(getVpxApiId);
   msgApi->ReleaseMsgID(onGameStartId);
   msgApi->ReleaseMsgID(onGameEndId);
   msgApi->ReleaseMsgID(vpxGameStartId);
   msgApi->ReleaseMsgID(vpxGameEndId);
   msgApi->ReleaseMsgID(onPrepareFrameId);

   msgApi = nullptr;
}