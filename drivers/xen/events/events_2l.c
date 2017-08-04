/*
 * Xen event channels (2-level ABI)
 *
 * Jeremy Fitzhardinge <jeremy@xensource.com>, XenSource Inc, 2007
 */

#define pr_fmt(fmt) "xen:" KBUILD_MODNAME ": " fmt

#include <linux/linkage.h>
#include <linux/interrupt.h>
#include <linux/irq.h>

#include <asm/sync_bitops.h>
#include <asm/xen/hypercall.h>
#include <asm/xen/hypervisor.h>

#include <xen/xen.h>
#include <xen/xen-ops.h>
#include <xen/events.h>
#include <xen/interface/xen.h>
#include <xen/interface/event_channel.h>

#include "events_internal.h"

/*
 * Note sizeof(xen_ulong_t) can be more than sizeof(unsigned long). Be
 * careful to only use bitops which allow for this (e.g
 * test_bit/find_first_bit and friends but not __ffs) and to pass
 * BITS_PER_EVTCHN_WORD as the bitmask length.
 */
#define BITS_PER_EVTCHN_WORD (sizeof(xen_ulong_t)*8)

/*
 * Make a bitmask (i.e. unsigned long *) of a xen_ulong_t
 * array. Primarily to avoid long lines (hence the terse name).
 */
#define BM(x) (unsigned long *)(x)
/* Find the first set bit in a evtchn mask */
#define EVTCHN_FIRST_BIT(w) find_first_bit(BM(&(w)), BITS_PER_EVTCHN_WORD)

#define EVTCHN_MASK_SIZE (EVTCHN_2L_NR_CHANNELS/BITS_PER_EVTCHN_WORD)

static DEFINE_PER_CPU(xen_ulong_t [EVTCHN_MASK_SIZE], cpu_evtchn_mask);

struct evtchn *evtchn_domains;
struct evtchn *evtchn_domain_local;

static void double_evtchn_lock(struct evtchn *lchn, struct evtchn *rchn)
{
    if ( lchn < rchn )
    {
        spin_lock(&lchn->lock);
        spin_lock(&rchn->lock);
    }
    else
    {
        if ( lchn != rchn )
            spin_lock(&rchn->lock);
        spin_lock(&lchn->lock);
    }
}

static void double_evtchn_unlock(struct evtchn *lchn, struct evtchn *rchn)
{
    spin_unlock(&lchn->lock);
    if ( lchn != rchn )
        spin_unlock(&rchn->lock);
}

static void free_evtchn( struct evtchn *chn)
{
    /* Clear pending event to avoid unexpected behavior on re-bind. */
    evtchn_ops->clear_pending(chn->port);

    /* Reset binding to vcpu0 when the channel is freed. */
    chn->state          = ECS_FREE;
    chn->notify_vcpu_id = 0;
    chn->xen_consumer   = 0;
}

static int evtchn_2l_get_free_port(void )
{
    int port;

    for ( port = 0; port_is_valid( port); port++ )
    {
        if ( port > EVTCHNS_PER_BUCKET)
            return -ENOSPC;
		if ( evtchn_from_port(evtchn_domain_local, port)->state == ECS_FREE )
            return port; 
    }
    return -ENOSPC;
}
static unsigned evtchn_2l_max_channels(void)
{
	return EVTCHN_2L_NR_CHANNELS;
}

static void evtchn_2l_bind_to_cpu(struct irq_info *info, unsigned cpu)
{
	clear_bit(info->evtchn, BM(per_cpu(cpu_evtchn_mask, info->cpu)));
	set_bit(info->evtchn, BM(per_cpu(cpu_evtchn_mask, cpu)));
}

static void evtchn_2l_clear_pending(unsigned port)
{
	struct shared_info *s = HYPERVISOR_shared_info;
	sync_clear_bit(port, BM(&s->evtchn_pending[0]));
}

static void evtchn_2l_set_pending(unsigned port)
{
	struct shared_info *s = HYPERVISOR_shared_info;
	sync_set_bit(port, BM(&s->evtchn_pending[0]));
}

static bool evtchn_2l_is_pending(unsigned port)
{
	struct shared_info *s = HYPERVISOR_shared_info;
	return sync_test_bit(port, BM(&s->evtchn_pending[0]));
}

static bool evtchn_2l_test_and_set_mask(unsigned port)
{
	struct shared_info *s = HYPERVISOR_shared_info;
	return sync_test_and_set_bit(port, BM(&s->evtchn_mask[0]));
}

static void evtchn_2l_mask(unsigned port)
{
	struct shared_info *s = HYPERVISOR_shared_info;
	sync_set_bit(port, BM(&s->evtchn_mask[0]));
}

static void evtchn_2l_unmask(unsigned port)
{
	struct shared_info *s = HYPERVISOR_shared_info;
	unsigned int cpu = get_cpu();
    unsigned int r_cpu = cpu_from_evtchn(port);
	int do_hypercall = 0, evtchn_pending = 0;

	BUG_ON(!irqs_disabled());

	if (unlikely((cpu != r_cpu)))
		do_hypercall = 1;
	else {
		/*
		 * Need to clear the mask before checking pending to
		 * avoid a race with an event becoming pending.
		 *
		 * EVTCHNOP_unmask will only trigger an upcall if the
		 * mask bit was set, so if a hypercall is needed
		 * remask the event.
		 */
		sync_clear_bit(port, BM(&s->evtchn_mask[0]));
		evtchn_pending = sync_test_bit(port, BM(&s->evtchn_pending[0]));

		if (unlikely(evtchn_pending && xen_hvm_domain())) {
			sync_set_bit(port, BM(&s->evtchn_mask[0]));
			do_hypercall = 1;
		}
	}

	/* Slow path (hypercall) if this is a non-local port or if this is
	 * an hvm domain and an event is pending (hvm domains don't have
	 * their own implementation of irq_enable). */
	if (do_hypercall) {
		//struct evtchn_unmask unmask = { .port = port };
		struct vcpu_info *vcpu_info = &HYPERVISOR_shared_info->vcpu_info[xen_vcpu_nr(r_cpu)];
		int already_pending;

		//(void)HYPERVISOR_event_channel_op(EVTCHNOP_unmask, &unmask);
		//(void)evtchn_unmask(port);
		if (!sync_test_and_set_bit(port / BITS_PER_EVTCHN_WORD,
					   BM(&vcpu_info->evtchn_pending_sel)) )
		{
			already_pending = sync_test_and_set_bit(port / BITS_PER_EVTCHN_WORD, BM(&vcpu_info->evtchn_upcall_pending));
			if ( !already_pending )
                 vcpu_info->evtchn_upcall_pending = 1;
		}

	} else {
		struct vcpu_info *vcpu_info = __this_cpu_read(xen_vcpu);

		/*
		 * The following is basically the equivalent of
		 * 'hw_resend_irq'. Just like a real IO-APIC we 'lose
		 * the interrupt edge' if the channel is masked.
		 */
		if (evtchn_pending &&
		    !sync_test_and_set_bit(port / BITS_PER_EVTCHN_WORD,
					   BM(&vcpu_info->evtchn_pending_sel)))
			vcpu_info->evtchn_upcall_pending = 1;
	}

	put_cpu();
}

static DEFINE_PER_CPU(unsigned int, current_word_idx);
static DEFINE_PER_CPU(unsigned int, current_bit_idx);

/*
 * Mask out the i least significant bits of w
 */
#define MASK_LSBS(w, i) (w & ((~((xen_ulong_t)0UL)) << i))

static inline xen_ulong_t active_evtchns(unsigned int cpu,
					 struct shared_info *sh,
					 unsigned int idx)
{
	return sh->evtchn_pending[idx] &
		per_cpu(cpu_evtchn_mask, cpu)[idx] &
		~sh->evtchn_mask[idx];
}

/*
 * Search the CPU's pending events bitmasks.  For each one found, map
 * the event number to an irq, and feed it into do_IRQ() for handling.
 *
 * Xen uses a two-level bitmap to speed searching.  The first level is
 * a bitset of words which contain pending event bits.  The second
 * level is a bitset of pending events themselves.
 */
static void evtchn_2l_handle_events(unsigned cpu)
{
	int irq;
	xen_ulong_t pending_words;
	xen_ulong_t pending_bits;
	int start_word_idx, start_bit_idx;
	int word_idx, bit_idx;
	int i;
	struct shared_info *s = HYPERVISOR_shared_info;
	struct vcpu_info *vcpu_info = __this_cpu_read(xen_vcpu);

	/* Timer interrupt has highest priority. */
	irq = irq_from_virq(cpu, VIRQ_TIMER);
	if (irq != -1) {
		unsigned int evtchn = evtchn_from_irq(irq);
		word_idx = evtchn / BITS_PER_LONG;
		bit_idx = evtchn % BITS_PER_LONG;
		if (active_evtchns(cpu, s, word_idx) & (1ULL << bit_idx))
			generic_handle_irq(irq);
	}

	/*
	 * Master flag must be cleared /before/ clearing
	 * selector flag. xchg_xen_ulong must contain an
	 * appropriate barrier.
	 */
	pending_words = xchg_xen_ulong(&vcpu_info->evtchn_pending_sel, 0);

	start_word_idx = __this_cpu_read(current_word_idx);
	start_bit_idx = __this_cpu_read(current_bit_idx);

	word_idx = start_word_idx;

	for (i = 0; pending_words != 0; i++) {
		xen_ulong_t words;

		words = MASK_LSBS(pending_words, word_idx);

		/*
		 * If we masked out all events, wrap to beginning.
		 */
		if (words == 0) {
			word_idx = 0;
			bit_idx = 0;
			continue;
		}
		word_idx = EVTCHN_FIRST_BIT(words);

		pending_bits = active_evtchns(cpu, s, word_idx);
		bit_idx = 0; /* usually scan entire word from start */
		/*
		 * We scan the starting word in two parts.
		 *
		 * 1st time: start in the middle, scanning the
		 * upper bits.
		 *
		 * 2nd time: scan the whole word (not just the
		 * parts skipped in the first pass) -- if an
		 * event in the previously scanned bits is
		 * pending again it would just be scanned on
		 * the next loop anyway.
		 */
		if (word_idx == start_word_idx) {
			if (i == 0)
				bit_idx = start_bit_idx;
		}

		do {
			xen_ulong_t bits;
			int port;

			bits = MASK_LSBS(pending_bits, bit_idx);

			/* If we masked out all events, move on. */
			if (bits == 0)
				break;

			bit_idx = EVTCHN_FIRST_BIT(bits);

			/* Process port. */
			port = (word_idx * BITS_PER_EVTCHN_WORD) + bit_idx;
			irq = get_evtchn_to_irq(port);

			if (irq != -1)
				generic_handle_irq(irq);

			bit_idx = (bit_idx + 1) % BITS_PER_EVTCHN_WORD;

			/* Next caller starts at last processed + 1 */
			__this_cpu_write(current_word_idx,
					 bit_idx ? word_idx :
					 (word_idx+1) % BITS_PER_EVTCHN_WORD);
			__this_cpu_write(current_bit_idx, bit_idx);
		} while (bit_idx != 0);

		/* Scan start_l1i twice; all others once. */
		if ((word_idx != start_word_idx) || (i != 0))
			pending_words &= ~(1UL << word_idx);

		word_idx = (word_idx + 1) % BITS_PER_EVTCHN_WORD;
	}
}

irqreturn_t xen_debug_interrupt(int irq, void *dev_id)
{
	struct shared_info *sh = HYPERVISOR_shared_info;
	int cpu = smp_processor_id();
	xen_ulong_t *cpu_evtchn = per_cpu(cpu_evtchn_mask, cpu);
	int i;
	unsigned long flags;
	static DEFINE_SPINLOCK(debug_lock);
	struct vcpu_info *v;

	spin_lock_irqsave(&debug_lock, flags);

	printk("\nvcpu %d\n  ", cpu);

	for_each_online_cpu(i) {
		int pending;
		v = per_cpu(xen_vcpu, i);
		pending = (get_irq_regs() && i == cpu)
			? xen_irqs_disabled(get_irq_regs())
			: v->evtchn_upcall_mask;
		printk("%d: masked=%d pending=%d event_sel %0*"PRI_xen_ulong"\n  ", i,
		       pending, v->evtchn_upcall_pending,
		       (int)(sizeof(v->evtchn_pending_sel)*2),
		       v->evtchn_pending_sel);
	}
	v = per_cpu(xen_vcpu, cpu);

	printk("\npending:\n   ");
	for (i = ARRAY_SIZE(sh->evtchn_pending)-1; i >= 0; i--)
		printk("%0*"PRI_xen_ulong"%s",
		       (int)sizeof(sh->evtchn_pending[0])*2,
		       sh->evtchn_pending[i],
		       i % 8 == 0 ? "\n   " : " ");
	printk("\nglobal mask:\n   ");
	for (i = ARRAY_SIZE(sh->evtchn_mask)-1; i >= 0; i--)
		printk("%0*"PRI_xen_ulong"%s",
		       (int)(sizeof(sh->evtchn_mask[0])*2),
		       sh->evtchn_mask[i],
		       i % 8 == 0 ? "\n   " : " ");

	printk("\nglobally unmasked:\n   ");
	for (i = ARRAY_SIZE(sh->evtchn_mask)-1; i >= 0; i--)
		printk("%0*"PRI_xen_ulong"%s",
		       (int)(sizeof(sh->evtchn_mask[0])*2),
		       sh->evtchn_pending[i] & ~sh->evtchn_mask[i],
		       i % 8 == 0 ? "\n   " : " ");

	printk("\nlocal cpu%d mask:\n   ", cpu);
	for (i = (EVTCHN_2L_NR_CHANNELS/BITS_PER_EVTCHN_WORD)-1; i >= 0; i--)
		printk("%0*"PRI_xen_ulong"%s", (int)(sizeof(cpu_evtchn[0])*2),
		       cpu_evtchn[i],
		       i % 8 == 0 ? "\n   " : " ");

	printk("\nlocally unmasked:\n   ");
	for (i = ARRAY_SIZE(sh->evtchn_mask)-1; i >= 0; i--) {
		xen_ulong_t pending = sh->evtchn_pending[i]
			& ~sh->evtchn_mask[i]
			& cpu_evtchn[i];
		printk("%0*"PRI_xen_ulong"%s",
		       (int)(sizeof(sh->evtchn_mask[0])*2),
		       pending, i % 8 == 0 ? "\n   " : " ");
	}

	printk("\npending list:\n");
	for (i = 0; i < EVTCHN_2L_NR_CHANNELS; i++) {
		if (sync_test_bit(i, BM(sh->evtchn_pending))) {
			int word_idx = i / BITS_PER_EVTCHN_WORD;
			printk("  %d: event %d -> irq %d%s%s%s\n",
			       cpu_from_evtchn(i), i,
			       get_evtchn_to_irq(i),
			       sync_test_bit(word_idx, BM(&v->evtchn_pending_sel))
			       ? "" : " l2-clear",
			       !sync_test_bit(i, BM(sh->evtchn_mask))
			       ? "" : " globally-masked",
			       sync_test_bit(i, BM(cpu_evtchn))
			       ? "" : " locally-masked");
		}
	}

	spin_unlock_irqrestore(&debug_lock, flags);

	return IRQ_HANDLED;
}

static void evtchn_2l_resume(void)
{
	int i;

	for_each_online_cpu(i)
		memset(per_cpu(cpu_evtchn_mask, i), 0, sizeof(xen_ulong_t) *
				EVTCHN_2L_NR_CHANNELS/BITS_PER_EVTCHN_WORD);
}

static void evtchn_2l_close(unsigned port)
{
    struct evtchn *chn1, *chn2, *evtchn_domain_remote;
	unsigned port2;
	chn1 = evtchn_from_port(evtchn_domain_local, port);

    switch ( chn1->state )
    {
    case ECS_FREE:
    case ECS_RESERVED:
	case ECS_PIRQ:
        goto out;

    case ECS_UNBOUND:
        break;

    case ECS_VIRQ:
        break;

    case ECS_IPI:
        break;

    case ECS_INTERDOMAIN:
        port2 = chn1->u.interdomain.remote_port;
		evtchn_domain_remote = evtchn_domains + (EVTCHNS_PER_BUCKET * chn1->u.interdomain.remote_dom) ;
        BUG_ON(!port_is_valid(port2));

        chn2 = evtchn_from_port(evtchn_domain_remote, port2);
        BUG_ON(chn2->state != ECS_INTERDOMAIN);

        double_evtchn_lock(chn1, chn2);

        free_evtchn( chn1);

        chn2->state = ECS_UNBOUND;
        chn2->u.unbound.remote_domid = CONFIG_XEN_DOM_ID;

        double_evtchn_unlock(chn1, chn2);

        goto out;

    default:
        BUG();
    }

    spin_lock(&chn1->lock);
    free_evtchn(chn1);
    spin_unlock(&chn1->lock);

 out:
	return;

}

static const struct evtchn_ops evtchn_ops_2l = {
	.max_channels      = evtchn_2l_max_channels,
	.nr_channels       = evtchn_2l_max_channels,
	.bind_to_cpu       = evtchn_2l_bind_to_cpu,
	.clear_pending     = evtchn_2l_clear_pending,
	.set_pending       = evtchn_2l_set_pending,
	.is_pending        = evtchn_2l_is_pending,
	.test_and_set_mask = evtchn_2l_test_and_set_mask,
	.mask              = evtchn_2l_mask,
	.unmask            = evtchn_2l_unmask,
	.handle_events     = evtchn_2l_handle_events,
	.resume	           = evtchn_2l_resume,
    .evtchn_close      = evtchn_2l_close,
    .get_free_port     = evtchn_2l_get_free_port,
};


static void setup_ports(void)
{
    unsigned int port;

    /*
     * For each port that is already bound:
     *
     * - save its pending state.
     * - set default priority.
     */
    for ( port = 1; port < EVTCHNS_PER_BUCKET ; port++ )
    {
        struct evtchn *evtchn;
		
        evtchn = evtchn_from_port(evtchn_domain_local, port);

        if (  HYPERVISOR_shared_info->evtchn_pending[port] == 1 )
            evtchn->pending = 1;

        evtchn->priority = EVTCHN_FIFO_PRIORITY_DEFAULT;
    }
}

void __init xen_evtchn_2l_init(void)
{
    int i;
    struct evtchn *event_channel_ptr;
	pr_info("Using 2-level ABI\n");
	evtchn_ops = &evtchn_ops_2l;

    // alloc_evtchn_bucket implementation
    evtchn_domains = xen_remap(0xfee21000, XEN_PAGE_SIZE * 2);
	if ( !evtchn_domains )
		   return;
	evtchn_domain_local = evtchn_domains + (EVTCHNS_PER_BUCKET * CONFIG_XEN_DOM_ID) ;
    memset(evtchn_domain_local, 0, EVTCHNS_PER_BUCKET * sizeof(*evtchn_domain_local));
    for ( i = 0; i < EVTCHNS_PER_BUCKET; i++ )
    {
       //Each port has an event channel structure associated with it
       event_channel_ptr = &evtchn_domain_local[i];
       event_channel_ptr->port = i;
       spin_lock_init(&event_channel_ptr->lock);
    }
    evtchn_from_port(evtchn_domain_local, 0)->state = ECS_RESERVED;
	
    setup_ports();

}
