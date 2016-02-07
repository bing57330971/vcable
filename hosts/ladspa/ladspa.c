#include "vcable.h"
#include <ladspa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MAX_PORTS 4
#define NUM_OUTPUTS (MAX_PORTS / 2)
#define MAX_SAMPLES 4096 // 4096 should be enough for everyone

struct instance {
   LADSPA_Data outbuf[NUM_OUTPUTS][MAX_SAMPLES];
   LADSPA_Data *portbuf[MAX_PORTS];
   struct vcable vcable;
   const LADSPA_Descriptor *desc;
   unsigned long sample_rate;
};

static struct {
   LADSPA_Descriptor desc[NUM_OUTPUTS];
} ladspa;

static void
write_cb(size_t port, const vcable_sample *buffer, size_t num_samples, uint32_t sample_rate, void *userdata)
{
   struct instance *instance = userdata;
   assert(instance);

   if (port >= NUM_OUTPUTS || instance->sample_rate != (unsigned long)sample_rate)
      return;

   const size_t w = (num_samples > MAX_SAMPLES ? MAX_SAMPLES : num_samples);
   assert(w <= MAX_SAMPLES);
   memcpy(instance->outbuf[port], buffer, w * sizeof(LADSPA_Data));
   memset(instance->outbuf[port] + w, 0, (MAX_SAMPLES - w) * sizeof(LADSPA_Data));
}

static void
process_cb(LADSPA_Handle userdata, unsigned long num_samples)
{
   struct instance *instance = userdata;
   assert(instance);

   for (size_t i = 0; i < instance->desc->PortCount; ++i) {
      const size_t vport = i / 2;
      if (i % 2) {
         // output
         const size_t w = (num_samples > MAX_SAMPLES ? MAX_SAMPLES : num_samples);
         memcpy(instance->portbuf[i], instance->outbuf[vport], w * sizeof(LADSPA_Data));
         memset(instance->outbuf[vport], 0, w * sizeof(LADSPA_Data));
      } else {
         // input
         vcable_write(&instance->vcable, vport, instance->portbuf[i], num_samples, instance->sample_rate);
      }
   }
}

static void
connect_cb(LADSPA_Handle userdata, unsigned long port, LADSPA_Data *buffer)
{
   struct instance *instance = userdata;
   assert(instance);

   if (port >= instance->desc->PortCount) {
      fprintf(stderr, "ladspa: connect() tried to connect to unknown port (%lu tried, %zu available)\n", port, instance->desc->PortCount);
      return;
   }

   instance->portbuf[port] = buffer;
}

static void
instance_release(struct instance *instance)
{
   if (!instance)
      return;

   vcable_release(&instance->vcable);
   free(instance);
}

static LADSPA_Handle
instance_cb(const LADSPA_Descriptor *desc, unsigned long sample_rate)
{
   struct instance *instance;
   if (!(instance = calloc(1, sizeof(*instance))))
      return NULL;

   instance->desc = desc;
   instance->sample_rate = sample_rate;

   if (!vcable_init(&instance->vcable))
      goto fail;

   const struct vcable_options opts = {
      .userdata = instance,
      .name = desc->Name,
      .write_cb = write_cb,
      .ports = desc->PortCount / 2,
      .sample_size = sizeof(LADSPA_Data),
   };

   vcable_set_options(&instance->vcable, &opts);

   // FIXME: Could do stepped integer control in LADSPA
   //        But that sucks since you don't see name of the loaded plugin..
   //        Until this becomes problem (we have more plugins than jack) just load the first one, yay.
   vcable_set_plugin(&instance->vcable, 1);
   return instance;

fail:
   instance_release(instance);
   return NULL;
}

static void
release_cb(LADSPA_Handle instance)
{
   instance_release(instance);
}

const LADSPA_Descriptor*
ladspa_descriptor(unsigned long i)
{
   return (i < NUM_OUTPUTS ? &ladspa.desc[i] : NULL);
}

__attribute__((constructor)) static void
init(void)
{
   static LADSPA_PortDescriptor ports[MAX_PORTS];
   for (size_t i = 0; i < MAX_PORTS; ++i)
      ports[i] = (i % 2 ? LADSPA_PORT_OUTPUT : LADSPA_PORT_INPUT) | LADSPA_PORT_AUDIO;

   static const char *names[MAX_PORTS] = { "input0", "output0", "input1", "output1" };
   static const char *labels[NUM_OUTPUTS] = { "vcable-mono", "vcable-stereo" };
   static const size_t num_ports[NUM_OUTPUTS] = { 2, 4 };

   for (size_t i = 0; i < NUM_OUTPUTS; ++i) {
      ladspa.desc[i].UniqueID = i;
      ladspa.desc[i].Properties = LADSPA_PROPERTY_HARD_RT_CAPABLE;
      ladspa.desc[i].Label = labels[i];
      ladspa.desc[i].Name = labels[i];
      ladspa.desc[i].Maker = "Jari Vetoniemi";
      ladspa.desc[i].Copyright = "None";
      ladspa.desc[i].PortCount = num_ports[i];
      ladspa.desc[i].PortDescriptors = ports;
      ladspa.desc[i].PortNames = names;
      ladspa.desc[i].instantiate = instance_cb;
      ladspa.desc[i].connect_port = connect_cb;
      ladspa.desc[i].run = process_cb;
      ladspa.desc[i].cleanup = release_cb;
   }
}
