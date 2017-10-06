/******************************************************************************
 * grant_table.c
 * ARM specific part
 *
 * Granting foreign access to our memory reservation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <xen/interface/xen.h>
#include <xen/page.h>
#include <xen/grant_table.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/io.h>

int base_value;
struct DataItem {
   void *data;   
   unsigned long key;
};

struct DataItem hashArray[SIZE_ARRAY]; 
#define WORDSIZE 64
#define MWORD_ALIGNMENT_MASK	((WORDSIZE >> 3) - 1)

void memcpy_xen(void *dest, const void *src, unsigned long length) {
	if (((unsigned long)dest | (unsigned long)src | length) & MWORD_ALIGNMENT_MASK) {
		unsigned char *_dest8 = dest;
		const unsigned char *_src8 = src;

		while (length > 0) {
			*(_dest8++) = *(_src8++);
			--length;
		}
	} else {
#if WORDSIZE == 32
		u32 *_dest32 = dest;
		const u32 *_src32 = src;

		while (length > 0) {
			*(_dest32++) = *(_src32++);
			length -= 4;
		}
#elif WORDSIZE == 64
		u64 *_dest64 = dest;
		const u64 *_src64 = src;

		while (length > 0) {
			*(_dest64++) = *(_src64++);
			length -= 8;
		}
#else
# error Unsupported word size.
#endif
	}
}

static void display(void);

int hashCode(unsigned long key) {
   return key % base_value;
}

void *hash_search(unsigned long key) {
   //get the hash 
   int hashIndex = hashCode(key);  
	
	
   if(hashArray[hashIndex].key == key)
         return hashArray[hashIndex].data; 
   else
       return NULL;        
}

static void hash_insert(unsigned long key,void *data) {


   //get the hash 
   int hashIndex = hashCode(key);
	
   hashArray[hashIndex].data = data;
   hashArray[hashIndex].key = key;
}

int arch_gnttab_map_shared(xen_pfn_t *frames, unsigned long nr_gframes,
			   unsigned long max_nr_gframes,
			   void **__shared)
{
	return -ENOSYS;
}

void arch_gnttab_unmap(void *shared, unsigned long nr_gframes)
{
	return;
}
static void display(void) {
   int i = 0;
	
   for(i = 0; i<SIZE_ARRAY; i++) {
	
      if(hashArray[i].data != NULL)
         printk(" (%lu,%lu)",hashArray[i].key,(unsigned long)hashArray[i].data);
      else
         printk(" ~~ ");
   }
	
   printk("\n");
}

int arch_gnttab_init(unsigned long nr_shared)
{
    void *virt_addr;
    int i;
    unsigned long phys_addr,key ;
#if CONFIG_XEN_DOM_ID == 1
    base_value = 1044224;
    phys_addr = 0xfef00000;
    
#elif CONFIG_XEN_DOM_ID == 0
    base_value = 1045248;
    phys_addr = 0xFF300000;

#endif
    
    
    virt_addr =  xen_remap( phys_addr , 0x400000);
    for (i = 0; i< SIZE_ARRAY; i++)
    {
        key = phys_addr >> XEN_PAGE_SHIFT; 
        //printk("key inserted is %lu for phys_addr \n",key, phys_addr);
        hash_insert(key, virt_addr);
        virt_addr = (unsigned long)(virt_addr) + 4096;
        phys_addr = phys_addr + 4096;
    }
    //display();
	return 0;
}
