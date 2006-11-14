/*  
 * Cisco C7200 (Predator) Simulation Platform.
 * Copyright (C) 2005-2006 Christophe Fillot.  All rights reserved.
 *
 * PA-MC-8TE1 card. Doesn't work at this time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <assert.h>

#include "mips64.h"
#include "dynamips.h"
#include "memory.h"
#include "device.h"
#include "net.h"
#include "net_io.h"
#include "ptask.h"
#include "dev_c7200.h"
#include "dev_plx.h"

/* Debugging flags */
#define DEBUG_ACCESS    1
#define DEBUG_UNKNOWN   1
#define DEBUG_TRANSMIT  1
#define DEBUG_RECEIVE   1

/* SSRAM */
#define SSRAM_START  0x10000
#define SSRAM_END    0x30000

/* PA-MC-8TE1 Data */
struct pa_mc_data {
   char *name;

   /* Virtual machine */
   vm_instance_t *vm;

   /* PCI device information */
   struct vdevice dev;
   struct pci_device *pci_dev;

   /* SSRAM device */
   struct vdevice ssram_dev;
   char *ssram_name;
   m_uint8_t ssram_data[0x20000];

   /* PLX9054 */
   vm_obj_t *plx_obj;

   /* NetIO descriptor */
   netio_desc_t *nio;

   /* TX ring scanner task id */
   ptask_id_t tx_tid;
};

/* Log a PA-MC-8TE1 message */
#define PA_MC_LOG(d,msg...) vm_log((d)->vm,(d)->name,msg)

/*
 * dev_ssram_access
 */
static void *dev_ssram_access(cpu_mips_t *cpu,struct vdevice *dev,
                              m_uint32_t offset,u_int op_size,u_int op_type,
                              m_uint64_t *data)
{
   struct pa_mc_data *d = dev->priv_data;

   if (op_type == MTS_READ)
      *data = 0;

   if ((offset >= SSRAM_START) && (offset < SSRAM_END))
      return(&d->ssram_data[offset-SSRAM_START]);

#if DEBUG_ACCESS
   if (op_type == MTS_READ) {
      cpu_log(cpu,d->name,
              "read  access to offset = 0x%x, pc = 0x%llx (size=%u)\n",
              offset,cpu->pc,op_size);
   } else {
      cpu_log(cpu,d->name,"write access to vaddr = 0x%x, pc = 0x%llx, "
              "val = 0x%llx (size=%u)\n",offset,cpu->pc,*data,op_size);
   }
#endif

   switch(offset) {      
      case 0xfff0c:
         if (op_type == MTS_READ)
            *data = 0xdeadbeef;
         break;

      case 0xfff10:
         if (op_type == MTS_READ)
            *data = 0xbeeffeed;
         break;

      case 0x08:  /* max_dsx1 */
      case 0x10:  /* no_buf */
      case 0x18:  /* ev */
         if (op_type == MTS_READ)
            *data = 0x0ULL;
         break;

      case 0x00:  /* tx packets */
         if (op_type == MTS_READ)
            *data = 0x0;
         break;

      case 0x04:  /* rx packets */
         if (op_type == MTS_READ)
            *data = 0x0;
         break;
         
      case 0x0c:  /* rx drops */
         if (op_type == MTS_READ)
            *data = 0;
         break;
   }

   return NULL;
}

/* Callback when PLX9054 PCI-to-Local register is written */
static void plx9054_doorbell_callback(struct plx_data *plx_data,
                                      struct pa_mc_data *pa_data,
                                      m_uint32_t val)
{
   printf("DOORBELL: 0x%x\n",val);

   /* Trigger interrupt */
   vm_set_irq(pa_data->vm,2);
   vm_set_irq(pa_data->vm,3);
}

/*
 * pa_mc8te1_access()
 */
static void *pa_mc8te1_access(cpu_mips_t *cpu,struct vdevice *dev,
                              m_uint32_t offset,u_int op_size,u_int op_type,
                              m_uint64_t *data)
{
   struct pa_mc_data *d = dev->priv_data;

   if (op_type == MTS_READ)
      *data = 0;

#if DEBUG_ACCESS
   if (op_type == MTS_READ) {
      cpu_log(cpu,d->name,"read  access to offset = 0x%x, pc = 0x%llx\n",
              offset,cpu->pc);
   } else {
      cpu_log(cpu,d->name,"write access to vaddr = 0x%x, pc = 0x%llx, "
              "val = 0x%llx\n",offset,cpu->pc,*data);
   }
#endif

   switch(offset) {

#if DEBUG_UNKNOWN
      default:
         if (op_type == MTS_READ) {
            cpu_log(cpu,d->name,
                    "read from unknown addr 0x%x, pc=0x%llx (size=%u)\n",
                    offset,cpu->pc,op_size);
         } else {
            cpu_log(cpu,d->name,
                    "write to unknown addr 0x%x, value=0x%llx, "
                    "pc=0x%llx (size=%u)\n",offset,*data,cpu->pc,op_size);
         }
#endif
   }

   return NULL;
}

/*
 * pci_pos_read()
 */
static m_uint32_t pci_pos_read(cpu_mips_t *cpu,struct pci_device *dev,int reg)
{
   struct pa_mc_data *d = dev->priv_data;

#if DEBUG_ACCESS
   PA_MC_LOG(d,"read PCI register 0x%x\n",reg);
#endif

   switch(reg) {
      case PCI_REG_BAR0:
         return(d->dev.phys_addr);
      default:
         return(0);
   }
}

/*
 * pci_pos_write()
 */
static void pci_pos_write(cpu_mips_t *cpu,struct pci_device *dev,
                          int reg,m_uint32_t value)
{
   struct pa_mc_data *d = dev->priv_data;

#if DEBUG_ACCESS
   PA_MC_LOG(d,"write 0x%x to PCI register 0x%x\n",value,reg);
#endif

   switch(reg) {
      case PCI_REG_BAR0:
         //vm_map_device(cpu->vm,&d->dev,(m_uint64_t)value);
         PA_MC_LOG(d,"registers are mapped at 0x%x\n",value);
         break;
   }
}

/*
 * dev_c7200_pa_mc8te1_init()
 *
 * Add a PA-MC-8TE1 port adapter into specified slot.
 */
int dev_c7200_pa_mc8te1_init(c7200_t *router,char *name,u_int pa_bay)
{
   struct pa_mc_data *d;

   /* Allocate the private data structure for PA-MC-8TE1 chip */
   if (!(d = malloc(sizeof(*d)))) {
      fprintf(stderr,"%s (PA-MC-8TE1): out of memory\n",name);
      return(-1);
   }

   memset(d,0,sizeof(*d));
   d->name = name;
   d->vm   = router->vm;

   /* Set the EEPROM */
   c7200_pa_set_eeprom(router,pa_bay,cisco_eeprom_find_pa("PA-MC-8TE1"));

   /* Create the PM7380 */
   d->pci_dev = pci_dev_add(router->pa_bay[pa_bay].pci_map,name,
                            0x11f8, 0x7380,
                            0,0,C7200_NETIO_IRQ,d,
                            NULL,pci_pos_read,pci_pos_write);

   /* Initialize SSRAM device */
   d->ssram_name = dyn_sprintf("%s_ssram",name);
   dev_init(&d->ssram_dev);
   d->ssram_dev.name      = d->ssram_name;
   d->ssram_dev.priv_data = d;
   d->ssram_dev.handler   = dev_ssram_access;

   /* Create the PLX9054 */
   d->plx_obj = dev_plx9054_init(d->vm,d->name,
                                 router->pa_bay[pa_bay].pci_map,1,
                                 &d->ssram_dev,NULL);

   /* Set callback function for PLX9054 PCI-To-Local doorbell */
   dev_plx_set_pci2loc_doorbell_cbk(d->plx_obj->data,
                                    (dev_plx_doorbell_cbk)
                                    plx9054_doorbell_callback,
                                    d);

   /* Store device info into the router structure */
   return(c7200_pa_set_drvinfo(router,pa_bay,d));
}

/* Remove a PA-POS-OC3 from the specified slot */
int dev_c7200_pa_mc8te1_shutdown(c7200_t *router,u_int pa_bay)
{
   struct c7200_pa_bay *bay;
   struct pa_mc_data *d;

   if (!(bay = c7200_pa_get_info(router,pa_bay)))
      return(-1);

   d = bay->drv_info;

   /* Remove the PA EEPROM */
   c7200_pa_unset_eeprom(router,pa_bay);

   /* Remove the PCI device */
   pci_dev_remove(d->pci_dev);

   /* Remove the PLX9054 chip */
   vm_object_remove(d->vm,d->plx_obj);

   /* Remove the device from the CPU address space */
   //vm_unbind_device(router->vm,&d->dev);
   vm_unbind_device(router->vm,&d->ssram_dev);

   cpu_group_rebuild_mts(router->vm->cpu_group);

   /* Free the device structure itself */
   free(d);
   return(0);
}

/* Bind a Network IO descriptor to a specific port */
int dev_c7200_pa_mc8te1_set_nio(c7200_t *router,u_int pa_bay,u_int port_id,
                                netio_desc_t *nio)
{
   struct pa_mc_data *d;

   if ((port_id > 0) || !(d = c7200_pa_get_drvinfo(router,pa_bay)))
      return(-1);

   if (d->nio != NULL)
      return(-1);

   d->nio = nio;
   //d->tx_tid = ptask_add((ptask_callback)dev_pos_oc3_handle_txring,d,NULL);
   //netio_rxl_add(nio,(netio_rx_handler_t)dev_pos_oc3_handle_rxring,d,NULL);
   return(0);
}

/* Bind a Network IO descriptor to a specific port */
int dev_c7200_pa_mc8te1_unset_nio(c7200_t *router,u_int pa_bay,u_int port_id)
{
   struct pa_mc_data *d;

   if ((port_id > 0) || !(d = c7200_pa_get_drvinfo(router,pa_bay)))
      return(-1);

   if (d->nio) {
      ptask_remove(d->tx_tid);
      netio_rxl_remove(d->nio);
      d->nio = NULL;
   }
   return(0);
}

/* PA-MC-8TE1 driver */
struct c7200_pa_driver dev_c7200_pa_mc8te1_driver = {
   "PA-MC-8TE1", 0, 
   dev_c7200_pa_mc8te1_init,
   dev_c7200_pa_mc8te1_shutdown,
   dev_c7200_pa_mc8te1_set_nio,
   dev_c7200_pa_mc8te1_unset_nio,
   NULL,
};