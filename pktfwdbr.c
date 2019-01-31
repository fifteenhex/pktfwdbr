#define GETTEXT_PACKAGE "gtk20"
#include <glib.h>
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include <mosquitto.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <mosquitto_client.h>

#include "pkt.h"

#include "config.h"

#define ERR_MQTTCONNECT 0
#define ERR_MQTTSUB 1
#define ERR_SOCK 2
#define ERR_RXADDR 3
#define ERR_TXADDR 4
#define ERR_RXBIND 5
#define ERR_TXCONNECT 6

#define TOPIC_ROOT		"pktfwdbr"
#define TOPIC_RX		"rx"
#define TOPIC_RX_JOIN	"join"
#define TOPIC_RX_UNCONF "unconfirmed"
#define TOPIC_RX_CONF	"confirmed"
#define TOPIC_RX_OTHER	"other"
#define TOPIC_TX		"tx"
#define TOPIC_TXACK		"txack"
#define TOPIC_STAT		"stat"

#define JSON_RXPK				"rxpk"
#define JSON_STAT				"stat"
#define JSON_GATEWAY_CONF		"gateway_conf"
#define JSON_GATEWAY_ID			"gateway_ID"
#define JSON_SERV_PORT_UP		"serv_port_up"
#define JSON_SERV_PORT_DOWN		"serv_port_down"

#define MTYPE_SHIFT 5
#define MTYPE_MASK 0b111
#define MTYPE_JOINREQ 0b000
#define MTYPE_UNCONFUP 0b010
#define MTYPE_CONFUP 0b100

struct context {
	GSocket* sock;
	GHashTable* forwarders;
	MosquittoClient* mosqclient;
};

struct publishcontext {
	gchar* id;
	MosquittoClient* mosqclient;
};

struct forwarder {
	const gchar* id;
	GSocketAddress* upstream;
	GSocketAddress* downsteam;
	guint64 lastseen;
	guint16 token;
	GHashTable* txtokens;
};

static gchar* createtopic(const gchar* id, ...) {
	GString* topicstr = g_string_new(TOPIC_ROOT"/");
	g_string_append(topicstr, id);

	va_list args;
	va_start(args, id);

	const gchar * part = va_arg(args, const gchar*);
	for (; part != NULL; part = va_arg(args, const gchar*)) {
		g_string_append(topicstr, "/");
		g_string_append(topicstr, part);
	}

	va_end(args);
	gchar* topic = g_string_free(topicstr, FALSE);
	return topic;
}

static gchar* extracteui(guchar* buffer) {
	GString* tmp = g_string_new(NULL);
	for (int i = 7; i >= 0; i--) {
		unsigned b = buffer[i];
		g_string_append_printf(tmp, "%02x", b);
	}
	return g_string_free(tmp, FALSE);
}

static void handlerx_processrx(JsonArray *array, guint index,
		JsonNode *element_node, gpointer data) {

	// Do a tiny bit of processing on the payload
	// so we can split joins etc onto a different
	// topic
	JsonObject* obj = json_node_get_object(element_node);
	const gchar* b64payload = json_object_get_string_member(obj, "data");
	gsize payloadlen;
	guchar* payload = g_base64_decode(b64payload, &payloadlen);
	uint8_t mtype = (*payload >> MTYPE_SHIFT) & MTYPE_MASK;

	gchar* subtopic;
	gchar* subsubtopic = NULL;
	gchar* subsubsubtopic = NULL;

	switch (mtype) {
	case MTYPE_JOINREQ: {
		subtopic = TOPIC_RX_JOIN;
		guchar* joinpayload = payload + 1;
		guchar* appeui = joinpayload;
		guchar* deveui = appeui + 8;
		subsubtopic = extracteui(appeui);
		subsubsubtopic = extracteui(deveui);
	}
		break;
	case MTYPE_UNCONFUP:
		subtopic = TOPIC_RX_UNCONF;
		break;
	case MTYPE_CONFUP:
		subtopic = TOPIC_RX_CONF;
		break;
	default:
		subtopic = TOPIC_RX_OTHER;
		break;
	}

	g_free(payload);

	struct publishcontext* cntx = (struct publishcontext*) data;
	gchar* topic = createtopic(cntx->id, TOPIC_RX, subtopic, subsubtopic,
			subsubsubtopic, NULL);

	mosquitto_client_publish_json(cntx->mosqclient, element_node, topic);

	g_free(topic);
	if (subsubtopic != NULL)
		g_free(subsubtopic);
	if (subsubsubtopic != NULL)
		g_free(subsubsubtopic);
}

static gchar* extractid(uint8_t* pktbuff) {
	GString* str = g_string_new(NULL);
	uint8_t* id = PKT_GATEWAYID(pktbuff);
	for (int i = 0; i < PKT_IDLEN; i++)
		g_string_append_printf(str, "%02"PRIx8, id[i]);
	return g_string_free(str, FALSE);
}

static struct forwarder* findforwarder(struct context* cntx, const gchar* id) {
	struct forwarder* forwarder = g_hash_table_lookup(cntx->forwarders, id);
	if (forwarder == NULL)
		g_message("don't know about forwarder %s", id);
	return forwarder;
}

static void subforgw(struct context* cntx, const gchar* id) {
	gchar* topic = createtopic(id, TOPIC_TX, "#", NULL);
	if (mosquitto_subscribe(
			mosquitto_client_getmosquittoinstance(cntx->mosqclient), NULL,
			topic, 0) != MOSQ_ERR_SUCCESS) {
		g_message("Failed to subscribe to topic");
	}
	g_free(topic);
}

static gboolean touchforwarder(struct context* cntx, const gchar* id,
		GSocketAddress* addr, gboolean downstream) {
	gboolean ret = false;

	struct forwarder* forwarder = g_hash_table_lookup(cntx->forwarders, id);
	if (forwarder == NULL) {
		if (g_hash_table_size(cntx->forwarders) < MAX_FORWARDERS) {
			id = g_strdup(id);
			forwarder = g_malloc(sizeof(*forwarder));
			forwarder->id = id;
			forwarder->txtokens = g_hash_table_new_full(g_direct_hash,
					g_direct_equal, NULL, (GDestroyNotify) g_free);
			g_hash_table_insert(cntx->forwarders, (gpointer) id, forwarder);
			subforgw(cntx, id);
		} else {
			g_message("can't track any more forwarders");
			goto out;
		}
	}

	if (downstream)
		forwarder->downsteam = addr;
	else
		forwarder->upstream = addr;
	forwarder->lastseen = g_get_monotonic_time();

	ret = true;

	out: return ret;
}

#define ERROR_JSON_TOOSHORT "json payload is too short"

static gboolean handlerx(GIOChannel *source, GIOCondition condition,
		gpointer data) {

	struct context* cntx = (struct context*) data;

	JsonParser* jsonparser = NULL;
	gchar* idstr = NULL;

	g_message("UDP incoming");

	gsize pktbuffsz = 8 * 1024;

	uint8_t* pktbuff = g_malloc(8 * 1024);
	if (pktbuff == NULL)
		goto out;

	GSocketAddress* theiraddr;
	gssize pktsz = g_socket_receive_from(cntx->sock, &theiraddr, pktbuff,
			pktbuffsz, NULL, NULL);
	if (pktsz == 0 || pktsz < sizeof(struct pkt_hdr)) {
		g_message("invalid packet size; %d", (int ) pktsz);
		goto out;
	}

	idstr = extractid(pktbuff);

	struct pkt_hdr* p = ((struct pkt_hdr*) pktbuff);
	if (!PKT_VALIDHEADER(p)) {
		g_message("invalid packet header");
		goto out;
	}

	gboolean downstream = (p->type == PKT_TYPE_PULL_DATA
			|| p->type == PKT_TYPE_TX_ACK);
	if (!touchforwarder(cntx, idstr, theiraddr, downstream))
		goto out;

	switch (p->type) {
	case PKT_TYPE_PUSH_DATA: {
		struct pkt_hdr ack = { .version = PKT_VERSION, .token = p->token,
				.type =
				PKT_TYPE_PUSH_ACK };

		if (g_socket_send_to(cntx->sock, theiraddr, (const gchar*) &ack,
				sizeof(ack), NULL, NULL) < 0)
			g_message("failed to ack push data");

		gssize jsonsz = PKT_JSONSZ(pktsz);

		if (jsonsz < 2) {
			g_message(ERROR_JSON_TOOSHORT);
			goto out;
		}

		g_message("Processing RX packets from %s", idstr);

		uint8_t* json = PKT_JSON(pktbuff);
		jsonparser = json_parser_new_immutable();
		if (!json_parser_load_from_data(jsonparser, (gchar*) json, jsonsz,
		NULL)) {
			g_message("failed to parse json");
			goto out;
		}
		JsonNode* root = json_parser_get_root(jsonparser);
		if (!JSON_NODE_HOLDS_OBJECT(root)) {
			g_message("json root should have been an object");
			goto out;
		}
		JsonObject* rootobj = json_node_get_object(root);

		if (json_object_has_member(rootobj, JSON_RXPK)) {
			JsonArray* rxpkts = json_object_get_array_member(rootobj,
			JSON_RXPK);
			struct publishcontext pcntx = { idstr, cntx->mosqclient };
			json_array_foreach_element(rxpkts, handlerx_processrx, &pcntx);
		} else
			g_message("no rx packets");

		if (json_object_has_member(rootobj, JSON_STAT)) {
			JsonObject* stat = json_object_get_object_member(rootobj,
			JSON_STAT);
		} else
			g_message("no stat");
	}
		break;
	case PKT_TYPE_PULL_DATA: {
		struct pkt_hdr ack = { .version = PKT_VERSION, .token = p->token,
				.type =
				PKT_TYPE_PULL_ACK };
		if (g_socket_send_to(cntx->sock, theiraddr, (const gchar*) &ack,
				sizeof(ack), NULL, NULL) < 0)
			g_message("failed to ack pull data");
	}
		break;
	case PKT_TYPE_TX_ACK: {
		g_message("got tx ack");
		uint8_t* json = PKT_JSON(pktbuff);
		gssize jsonsz = PKT_JSONSZ(pktsz);
		if (jsonsz < 2) {
			g_message(ERROR_JSON_TOOSHORT);
			jsonsz = 0;
		}

		if (jsonsz == 0)
			json = NULL;

		struct forwarder* forwarder = findforwarder(cntx, idstr);

		gchar* txtoken = g_hash_table_lookup(forwarder->txtokens,
				GINT_TO_POINTER(p->token));
		gchar* topic = createtopic(idstr, TOPIC_TXACK, txtoken, NULL);
		mosquitto_publish(
				mosquitto_client_getmosquittoinstance(cntx->mosqclient), NULL,
				topic, jsonsz, json, 0, false);
		g_hash_table_remove(forwarder->txtokens, GINT_TO_POINTER(p->token));
	}
		break;
	default:
		g_message("Unhandled type: %d", (int ) p->type);
		goto out;
	}

	out: if (pktbuff != NULL)
		g_free(pktbuff);
	if (idstr != NULL)
		g_free(idstr);
	if (jsonparser != NULL)
		g_object_unref(jsonparser);

	return TRUE;
}

static gboolean messagecallback(MosquittoClient* client,
		const struct mosquitto_message* message, gpointer userdata) {
	struct context* cntx = userdata;

	char** splittopic = NULL;
	int topicparts;
	uint8_t* pkt = NULL;

	mosquitto_sub_topic_tokenise(message->topic, &splittopic, &topicparts);

	if (topicparts < 3) {
		g_message("not enough topic parts");
		goto out;
	}

	char* root = splittopic[0];
	char* gatewayid = splittopic[1];
	char* direction = splittopic[2];

	if (strcmp(root, TOPIC_ROOT) != 0) {
		g_message("bad topic root");
		goto out;
	}

	if (strcmp(direction, TOPIC_TX) != 0) {
		g_message("unexpected direction");
		goto out;
	}

	g_message("have tx packet");

	struct forwarder* forwarder = findforwarder(cntx, gatewayid);
	if (forwarder == NULL)
		return TRUE;
	if (forwarder->downsteam == NULL) {
		g_message("don't know downstream address yet");
		goto out;
	}

	uint16_t token = forwarder->token++;

	if (topicparts >= 4) {
		char* txtoken = strdup(splittopic[3]);
		g_message("tracking tx with token %s", txtoken);
		g_hash_table_insert(forwarder->txtokens, GINT_TO_POINTER(token),
				txtoken);
	}

	gsize pktsz = sizeof(struct pkt_hdr) + message->payloadlen;
	pkt = g_malloc(pktsz);

	struct pkt_hdr hdr = { .version = PKT_VERSION, .token = token, .type =
	PKT_TYPE_PULL_RESP };

	memcpy(pkt, &hdr, sizeof(hdr));
	memcpy(pkt + sizeof(hdr), message->payload, message->payloadlen);

	if (g_socket_send_to(cntx->sock, forwarder->downsteam, (const gchar*) pkt,
			pktsz, NULL, NULL) < 0)
		g_message("failed to send pull resp");

	out: if (splittopic != NULL)
		mosquitto_sub_topic_tokens_free(&splittopic, topicparts);
	if (pkt != NULL)
		g_free(pkt);

	return TRUE;
}

static void connectedcallback_resub(gpointer key, gpointer value,
		gpointer userdata) {
	struct forwarder* forwarder = value;
	struct context* cntx = (struct context*) userdata;
	subforgw(cntx, forwarder->id);
}

static void connectedcallback(MosquittoClient* client, void* something,
		gpointer userdata) {
	struct context* cntx = (struct context*) userdata;

	g_hash_table_foreach(cntx->forwarders, connectedcallback_resub, cntx);
}

int main(int argc, char** argv) {

	int ret = 0;

	struct context cntx = { 0 };
	cntx.forwarders = g_hash_table_new(g_str_hash, g_str_equal);

	gchar* mqttid = NULL;
	gchar* mqtthost = "localhost";
	gint mqttport = 1883;
	gchar* mqttrootcert = NULL;
	gchar* mqttdevicecert = NULL;
	gchar* mqttdevicekey = NULL;
	gint listenport = 1912;
	GOptionEntry entries[] = { //
			MQTTOPTS, { "listenport", 'l', 0, G_OPTION_ARG_INT, &listenport, "",
					"" }, { NULL } };

	GOptionContext* context = g_option_context_new("");
	GError* error = NULL;
	g_option_context_add_main_entries(context, entries, GETTEXT_PACKAGE);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_print("option parsing failed: %s\n", error->message);
		goto out;
	}

	g_message("using mqtt broker at %s on port %d", mqtthost, mqttport);

	cntx.mosqclient = mosquitto_client_new_plaintext(mqttid, mqtthost,
			mqttport);

	g_signal_connect(cntx.mosqclient, MOSQUITTO_CLIENT_SIGNAL_CONNECTED,
			(GCallback )connectedcallback, &cntx);

	g_signal_connect(cntx.mosqclient, MOSQUITTO_CLIENT_SIGNAL_MESSAGE,
			(GCallback )messagecallback, &cntx);

	GInetAddress* loinetaddr = g_inet_address_new_loopback(
			G_SOCKET_FAMILY_IPV4);

	cntx.sock = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
			G_SOCKET_PROTOCOL_DEFAULT, NULL);
	if (cntx.sock == NULL) {
		ret = ERR_SOCK;
		goto out;
	}

	g_message("listening for packet forwarder packets on port %d", listenport);
	GSocketAddress* rxaddr = g_inet_socket_address_new(loinetaddr, listenport);
	if (rxaddr == NULL) {
		ret = ERR_RXADDR;
		goto out;
	}

	if (!g_socket_bind(cntx.sock, rxaddr, FALSE, NULL)) {
		ret = ERR_RXBIND;
		goto out;
	}

	int rxfd = g_socket_get_fd(cntx.sock);
	GIOChannel* rxchan = g_io_channel_unix_new(rxfd);
	g_io_add_watch(rxchan, G_IO_IN, handlerx, &cntx);

	GMainLoop* mainloop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(mainloop);

	out: return ret;
}
