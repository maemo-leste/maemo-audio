/* Write a route volume directly, so the announce path can be watched from the
 * outside: run with flags 0 and flags 1 (SPA_DEVICE_SET_PARAM_FLAG_TRANSIENT)
 * and compare what wireplumber does. argv: <sink node name> <linear volume>
 * [flags] [save 0|1]. */
#include <pipewire/pipewire.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/pod/pod.h>
#include <spa/pod/vararg.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SINK;
static float WANT;
static uint32_t FLAGS;
static int SAVE = 1;
static int REPEAT;
static int rep_i;
static GMainLoop *loop;
static struct pw_registry *reg;
static struct pw_proxy *dev_proxy;
static struct pw_proxy *sink_proxy;
static uint32_t want_device_id;
static uint32_t want_dev_idx;
static uint32_t active_port = SPA_ID_INVALID;
static int stage;

static void dump_route(const char *tag, const struct spa_pod *param)
{
  uint32_t index = 0, dev = 0;
  const struct spa_pod *props = NULL;
  if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, NULL,
        SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
        SPA_PARAM_ROUTE_device, SPA_POD_Int(&dev),
        SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&props)) < 0) {
    g_print("  %s: unparseable\n", tag);
    return;
  }
  g_print("  %s: route_index=%u device=%u props=%s\n", tag, index, dev,
          props ? "yes" : "no");
  if (props) {
    const struct spa_pod_prop *p =
        spa_pod_object_find_prop((const struct spa_pod_object *) props, NULL,
                               SPA_PROP_channelVolumes);
    if (p) {
      float v[8] = {0};
      uint32_t n = spa_pod_copy_array(&p->value, SPA_TYPE_Float, v, 8);
      g_print("      channelVolumes[0..%u] = %f\n", n, v[0]);
    }
  }
}

static void dev_param(void *d, int seq, uint32_t id, uint32_t index,
                     uint32_t next, const struct spa_pod *param)
{
  (void) d; (void) seq; (void) next; (void) index;
  if (id == SPA_PARAM_Route) {
    uint32_t dev = 0;
    if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, NULL,
          SPA_PARAM_ROUTE_device, SPA_POD_Int(&dev)) >= 0 && dev == want_dev_idx)
      spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, NULL,
          SPA_PARAM_ROUTE_index, SPA_POD_Int(&active_port));
    dump_route("device Route", param);
  }
}

static const struct pw_device_events dev_events = {
  PW_VERSION_DEVICE_EVENTS, .param = dev_param,
};

static void write_route(void)
{
  uint8_t buf[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
  struct spa_pod_frame f[2];
  float vals[2] = { WANT, WANT };

  spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
  spa_pod_builder_add(&b,
      SPA_PARAM_ROUTE_index, SPA_POD_Int(active_port),
      SPA_PARAM_ROUTE_device, SPA_POD_Int(want_dev_idx), 0);
  spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
  spa_pod_builder_push_object(&b, &f[1], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
  spa_pod_builder_add(&b, SPA_PROP_channelVolumes,
      SPA_POD_Array(sizeof(float), SPA_TYPE_Float, 2, vals), 0);
  spa_pod_builder_pop(&b, &f[1]);
  if (SAVE) {
    spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_save, 0);
    spa_pod_builder_bool(&b, true);
  }
  struct spa_pod *param = spa_pod_builder_pop(&b, &f[0]);

  g_print("  WRITE route index=%u device=%u lin=%f flags=%u save=%d -> %d\n",
          active_port, want_dev_idx, WANT, FLAGS, SAVE,
          pw_device_set_param((struct pw_device *) dev_proxy, SPA_PARAM_Route, FLAGS, param));
}

static gboolean tick(gpointer d)
{
  (void) d;
  /* Repeat mode: one write per tick, alternating between two levels so every
   * write is a real change (an unchanged level is not announced and would
   * make a cost measurement lie). Used to compare wireplumber's work per
   * route write with and without SPA_DEVICE_SET_PARAM_FLAG_TRANSIENT. */
  if (REPEAT > 0) {
    if (stage >= 3) {
      if (active_port == SPA_ID_INVALID) {
        if (stage > 10) { g_print("  no active port\n"); g_main_loop_quit(loop); return G_SOURCE_REMOVE; }
        return G_SOURCE_CONTINUE;
      }
      if (rep_i++ >= REPEAT) { g_main_loop_quit(loop); return G_SOURCE_REMOVE; }
      WANT = (rep_i % 2) ? 0.30f : 0.70f;
      write_route();
      stage++;
      return G_SOURCE_CONTINUE;
    }
  }
  switch (stage++) {
  case 1:
    if (dev_proxy) {
      uint32_t ids[] = { SPA_PARAM_Route };
      pw_device_subscribe_params((struct pw_device *) dev_proxy, ids, 1);
      pw_device_enum_params((struct pw_device *) dev_proxy, 0, SPA_PARAM_Route, 0, UINT32_MAX, NULL);
    } else {
      g_print("  no device proxy\n");
    }
    break;
  case 3:
    if (active_port == SPA_ID_INVALID) { g_print("  no active port\n"); break; }
    write_route();
    break;
  case 6:
    g_print("  re-enum after write\n");
    pw_device_enum_params((struct pw_device *) dev_proxy, 0, SPA_PARAM_Route, 0, UINT32_MAX, NULL);
    break;
  case 8:
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void node_info(void *d, const struct pw_node_info *info)
{
  (void) d;
  if (dev_proxy != NULL || info->props == NULL) return;
  const char *di  = spa_dict_lookup(info->props, "device.id");
  const char *cpd = spa_dict_lookup(info->props, "card.profile.device");
  if (!di || !cpd) return;
  want_device_id = atoi(di);
  want_dev_idx = atoi(cpd);
  g_print("  sink info -> device.id=%u card.profile.device=%u\n",
          want_device_id, want_dev_idx);
  dev_proxy = pw_registry_bind(reg, want_device_id,
                             PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0);
  g_print("  bind Device(%u) -> %p\n", want_device_id, (void*) dev_proxy);
  if (!dev_proxy) return;
  static struct spa_hook dev_hook;
  pw_proxy_add_object_listener(dev_proxy, &dev_hook, &dev_events, NULL);
}

static const struct pw_node_events node_events = {
  PW_VERSION_NODE_EVENTS, .info = node_info,
};

static void on_global(void *data, uint32_t id, uint32_t perms, const char *type,
                     uint32_t version, const struct spa_dict *props)
{
  (void) version;
  g_print("  GLOBAL id=%u type=%s perms=%08x name=%s\n", id, type, perms,
          spa_dict_lookup(props, "device.name") ?: spa_dict_lookup(props, "node.name") ?: "?");
  if (g_strcmp0(type, PW_TYPE_INTERFACE_Node) == 0) {
    const char *n = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!n || g_strcmp0(n, SINK) != 0) return;
    g_print("  sink global id=%u perms=%08x\n", id, perms);
    sink_proxy = pw_registry_bind((struct pw_registry *) data, id,
                                PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
    if (!sink_proxy) { g_print("  sink bind failed\n"); return; }
    static struct spa_hook sink_hook;
    pw_proxy_add_object_listener(sink_proxy, &sink_hook, &node_events, NULL);
  }
}

static const struct pw_registry_events reg_events = {
  PW_VERSION_REGISTRY_EVENTS, .global = on_global,
};

static gboolean bridge(GIOChannel *c, GIOCondition cond, gpointer d)
{ (void) c; (void) cond; pw_loop_iterate((struct pw_loop *) d, false); return G_SOURCE_CONTINUE; }

int main(int argc, char **argv)
{
  SINK = argc > 1 ? argv[1] : "alsa_output.pci-0000_00_05.0.analog-stereo";
  WANT = argc > 2 ? (float) atof(argv[2]) : 0.25f;
  FLAGS = argc > 3 ? (uint32_t) strtoul(argv[3], NULL, 0) : 0;
  SAVE = argc > 4 ? atoi(argv[4]) : 1;
  REPEAT = argc > 5 ? atoi(argv[5]) : 0;
  setbuf(stdout, NULL);
  pw_init(NULL, NULL);
  struct pw_main_loop *ml = pw_main_loop_new(NULL);
  struct pw_loop *l = pw_main_loop_get_loop(ml);
  struct pw_context *ctx = pw_context_new(l, NULL, 0);
  struct pw_core *core = pw_context_connect(ctx, NULL, 0);
  reg = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener(reg, &(struct spa_hook){0}, &reg_events, reg);
  pw_core_sync(core, PW_ID_CORE, 0);
  loop = g_main_loop_new(NULL, FALSE);
  GIOChannel *ch = g_io_channel_unix_new(pw_loop_get_fd(l));
  g_io_channel_set_close_on_unref(ch, FALSE);
  GSource *w = g_io_create_watch(ch, G_IO_IN);
  g_source_set_callback(w, (GSourceFunc) bridge, l, NULL);
  g_source_attach(w, g_main_context_default());
  g_timeout_add(500, tick, NULL);
  g_main_loop_run(loop);
  return 0;
}
