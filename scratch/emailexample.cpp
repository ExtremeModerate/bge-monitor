/* ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Tito Boro(vski) <t_i_t_t_o@email.com> wrote this file. As long as you retain
 * this notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return.
 *
 * 25.10.2015 Sofia, Bulgaria
 * ----------------------------------------------------------------------------*/
#include <ip_addr.h>
#include <espconn.h>
#include <mem.h>
#include <osapi.h>

/* Built-in ROM functions */
extern void mem_init(void *);
extern void mem_free(void *);
extern void * base64_encode(const unsigned char *in, size_t len);
void *pvPortZalloc(size_t, const char *file, int line);
void vPortFree(void *ptr, const char *file, int line);
char *ets_strcpy(char *dest, const char *src   );
void *ets_memcpy(void *dest, const void *src, size_t n);
int ets_sprintf(char *str, const char *format, ...)  __attribute__ ((format (printf, 2, 3)));
int os_printf_plus(const char *format, ...)  __attribute__ ((format (printf, 1, 2)));

#define SMTP_MAX_RETRIES    5
#define SMTP_MAX_STRING    32
#define SMTP_MAX_BODY       64

static void ICACHE_FLASH_ATTR smtp_dns_found_cb(const char *name, ip_addr_t *ipaddr, void *arg);

typedef enum
{
   SMTP_STATE_HELO,
   SMTP_STATE_AUTH,
   SMTP_STATE_USER,
   SMTP_STATE_PASS,
   SMTP_STATE_FROM,
   SMTP_STATE_RCPT,
   SMTP_STATE_DATA,
   SMTP_STATE_SUBJ_HEAD,
   SMTP_STATE_SUBJ_BODY,
   SMTP_STATE_SUBJ_FOOT,
   SMTP_STATE_MAIL_BODY,
   SMTP_STATE_MAIL_FOOT,
} SMTP_STATE;

typedef struct
{
   struct espconn    con; // MUST be first!!!
   esp_tcp       tcp;
   int            retries;
   SMTP_STATE       state;
   char          server[SMTP_MAX_STRING];
   char          user[SMTP_MAX_STRING];
   char          pass[SMTP_MAX_STRING];
   char          rcpt[SMTP_MAX_STRING];
   char          subject[SMTP_MAX_STRING];
   char          body[SMTP_MAX_BODY];
} smtp_conn_t;

unsigned char *default_certificate;
unsigned int default_certificate_len = 0;
unsigned char *default_private_key;
unsigned int default_private_key_len = 0;

static void base64encode(size_t in_len, const unsigned char *in, size_t out_len, char *out)
{
   void *pp, *mm = os_zalloc(0x1000 + 32);
   mem_init(mm);
   pp = base64_encode(in, in_len);
   os_strcpy(out, pp);
   os_strcpy(os_strchr(out, '\n'), "\r\n");
   os_free(mm);
}

static void smtp_send_data(smtp_conn_t *smtp_conn, char *data)
{
   #ifdef PLATFORM_DEBUG
   os_printf(data);
   #endif
    if (smtp_conn->con.proto.tcp->remote_port == 465)
      espconn_secure_send(&smtp_conn->con, (uint8 *)data, strlen(data));
   else
      espconn_send(&smtp_conn->con, (uint8 *)data, strlen(data));
}

static void ICACHE_FLASH_ATTR smtp_receive_cb(void *arg, char *pdata, unsigned short len)
{
   smtp_conn_t *smtp_conn = arg;
   char buff[SMTP_MAX_STRING + 16];

   #ifdef PLATFORM_DEBUG
   pdata[len] = 0;
   os_printf(pdata);
   #endif

   switch (smtp_conn->state)
   {
   case SMTP_STATE_HELO:
      os_sprintf(buff, "HELO %s\r\n", smtp_conn->server);
      break;

   case SMTP_STATE_AUTH:
      strncpy(buff, "AUTH LOGIN\r\n", sizeof(buff));
      break;

   case SMTP_STATE_USER:
      base64encode(strlen(smtp_conn->user), (unsigned char *)smtp_conn->user, sizeof(buff), buff);
      break;

   case SMTP_STATE_PASS:
      base64encode(strlen(smtp_conn->user), (unsigned char *)smtp_conn->pass, sizeof(buff), buff);
      break;

   case SMTP_STATE_FROM:
      os_sprintf(buff, "MAIL FROM:<%s>\r\n", smtp_conn->user);
      break;

   case SMTP_STATE_RCPT:
      os_sprintf(buff, "RCPT TO:<%s>\r\n", smtp_conn->rcpt);
      break;

   case SMTP_STATE_DATA:
      strncpy(buff, "DATA\r\n", sizeof(buff));
      break;

   case SMTP_STATE_SUBJ_HEAD:
      if (*smtp_conn->subject)
      {
         strncpy(buff, "Subject:", sizeof(buff));
      }
      else
      {
         smtp_conn->state = SMTP_STATE_MAIL_FOOT;
         if (*smtp_conn->body)
         {
            smtp_send_data(smtp_conn, smtp_conn->body);
            return;
         }
         else
            strncpy(buff, "\r\n.\r\nQUIT\r\n", sizeof(buff));
      }
      break;

   default:
      return;
   }

   smtp_send_data(smtp_conn, buff);
   smtp_conn->state++;
}

static void ICACHE_FLASH_ATTR smtp_sent_cb(void *arg)
{
   smtp_conn_t *smtp_conn = arg;
   char buff[SMTP_MAX_STRING + 16];

   switch (smtp_conn->state)
   {
   case SMTP_STATE_SUBJ_BODY:
      strncpy(buff, smtp_conn->subject, sizeof(buff));
      if (!*smtp_conn->body)
         smtp_conn->state = SMTP_STATE_MAIL_BODY;
      break;

   case SMTP_STATE_SUBJ_FOOT:
      strncpy(buff, "\r\n", sizeof(buff));
      break;

   case SMTP_STATE_MAIL_BODY:
      strncpy(buff, smtp_conn->body, sizeof(buff));
      break;

   case SMTP_STATE_MAIL_FOOT:
      strncpy(buff, "\r\n.\r\nQUIT\r\n", sizeof(buff));
      break;

   default:
      return;
   }

   smtp_send_data(smtp_conn, buff);
   smtp_conn->state++;
}

static void ICACHE_FLASH_ATTR smtp_disconnect_cb(void *arg)
{
   os_free(arg);
   #ifdef PLATFORM_DEBUG
   os_printf("SMTP disconnected\r\n");
   #endif
}

static void ICACHE_FLASH_ATTR smtp_reconnect_cb(void *arg, sint8 err)
{
   smtp_conn_t *smtp_conn = arg;

   os_printf("SMTP disconnected (err %d), ", err);
   if (++smtp_conn->retries > SMTP_MAX_RETRIES)
   {
      #ifdef PLATFORM_DEBUG
      os_printf("giving up!\r\n");
      #endif
      os_free(arg);
   }
   else
   {
      #ifdef PLATFORM_DEBUG
      os_printf("retrying...\r\n");
      #endif
      smtp_dns_found_cb(smtp_conn->server, (ip_addr_t *)&smtp_conn->con.proto.tcp->remote_ip, smtp_conn);
   }
}

static void ICACHE_FLASH_ATTR smtp_connect_cb(void *arg)
{
   #ifdef PLATFORM_DEBUG
   os_printf("SMTP connected\r\n");
   #endif
   espconn_regist_recvcb(arg, smtp_receive_cb);
   espconn_regist_sentcb(arg, smtp_sent_cb);
   espconn_regist_disconcb(arg, smtp_disconnect_cb);
}

static void ICACHE_FLASH_ATTR smtp_dns_found_cb(const char *name, ip_addr_t *ipaddr, void *arg)
{
   smtp_conn_t *smtp_conn = arg;

   if(ipaddr == NULL)
   {
      #ifdef PLATFORM_DEBUG
      os_printf("SMTP DNS lookup failed\r\n");
      #endif
      smtp_disconnect_cb(smtp_conn);
      return;
   }

   os_memcpy(smtp_conn->con.proto.tcp->remote_ip, &ipaddr->addr, 4);
   espconn_regist_connectcb(&smtp_conn->con, smtp_connect_cb);
   espconn_regist_reconcb(arg, smtp_reconnect_cb);
   if (smtp_conn->con.proto.tcp->remote_port == 465)
   {
      espconn_secure_ca_disable(3);
      espconn_secure_set_size(3, 8 * 1024);
      espconn_secure_connect(&smtp_conn->con);
   }
   else
      espconn_connect(&smtp_conn->con);

   #ifdef PLATFORM_DEBUG
   os_printf("SMTP connecting to " IPSTR ":%d\r\n",
         IP2STR(smtp_conn->con.proto.tcp->remote_ip),
         smtp_conn->con.proto.tcp->remote_port);
   #endif
}

void ICACHE_FLASH_ATTR smtp_send(const char *server, const int server_port,
               const char *user, const char *pass,
               const char *rcpt, const char *subject, const char *body)
{
   smtp_conn_t *smtp_conn = (smtp_conn_t *)os_zalloc(sizeof(smtp_conn_t));
   if (smtp_conn == NULL)
   {
      #ifdef PLATFORM_DEBUG
      os_printf("SMTP mem alloc failed\r\n");
      #endif
      return;
   }

   strncpy(smtp_conn->server, server, sizeof(smtp_conn->server));
   strncpy(smtp_conn->user, user, sizeof(smtp_conn->user));
   strncpy(smtp_conn->pass, pass, sizeof(smtp_conn->pass));
   strncpy(smtp_conn->rcpt, rcpt, sizeof(smtp_conn->rcpt));
   strncpy(smtp_conn->subject, subject? subject: "", sizeof(smtp_conn->subject));
   strncpy(smtp_conn->body, body? body: "", sizeof(smtp_conn->body));

   smtp_conn->con.type = ESPCONN_TCP;
   smtp_conn->con.state = ESPCONN_NONE;
   smtp_conn->con.proto.tcp = &smtp_conn->tcp;
   smtp_conn->con.proto.tcp->local_port = espconn_port();
   smtp_conn->con.proto.tcp->remote_port = server_port;

   ip_addr_t ip_addr;
    switch (espconn_gethostbyname(&smtp_conn->con, server, &ip_addr, smtp_dns_found_cb))
   {
   case ESPCONN_INPROGRESS:
      #ifdef PLATFORM_DEBUG
      os_printf("SMTP DNS lookup for %s\r\n", server);
      #endif
      break;

   case ESPCONN_OK:
      smtp_dns_found_cb(server, &ip_addr, smtp_conn);
      break;

   case ESPCONN_ARG:
      #ifdef PLATFORM_DEBUG
      os_printf("SMTP DNS argument error %s\n", server);
      #endif
      break;

   default:
      #ifdef PLATFORM_DEBUG
      os_printf("SMTP DNS lookup error\r\n");
      #endif
      break;
   }
}
//- See more at: http://www.esp8266.com/viewtopic.php?f=6&t=6173#sthash.guoi4N4k.dpuf
