/*  wcpAGCpair.c

Paired AGC for the diversity ear split: two receive channels share one
AGC gain. Added to WDSP, not changed in it: wcpAGC.c carries only the
pair/slot fields' use in xwcpagc() (one line) and destroy_wcpagc() (one
line), and the single AGC's code there is the original, untouched. The
per-sample arithmetic below is therefore a copy of xwcpagc()'s, split
into the same three steps; keep the two in step if upstream changes it.

*/

#include "comm.h"

#define AGCPAIR_TIMEOUT_MS 100

typedef struct _agcpair
{
	WCPAGC m[2];
	int chan[2];					// the two channels, to ask whether the partner is running
	volatile long arrived;
	HANDLE done[2];
} agcpair;

static agcpair agc_pair;

//
// The AGC in three per-sample steps, as in xwcpagc(), so that a pair can
// feed the state machine something other than one arm's own detector.
//
static void wcpagc_detect (WCPAGC a, int i)
{
	int j, k;
	if (++a->out_index >= a->ring_buffsize)
		a->out_index -= a->ring_buffsize;
	if (++a->in_index >= a->ring_buffsize)
		a->in_index -= a->ring_buffsize;

	a->out_sample[0] = a->ring[2 * a->out_index + 0];
	a->out_sample[1] = a->ring[2 * a->out_index + 1];
	a->abs_out_sample = a->abs_ring[a->out_index];
	a->ring[2 * a->in_index + 0] = a->in[2 * i + 0];
	a->ring[2 * a->in_index + 1] = a->in[2 * i + 1];
	if (a->pmode == 0)
		a->abs_ring[a->in_index] = max(fabs(a->ring[2 * a->in_index + 0]), fabs(a->ring[2 * a->in_index + 1]));
	else
		a->abs_ring[a->in_index] = sqrt(a->ring[2 * a->in_index + 0] * a->ring[2 * a->in_index + 0] + a->ring[2 * a->in_index + 1] * a->ring[2 * a->in_index + 1]);

	a->fast_backaverage = a->fast_backmult * a->abs_out_sample + a->onemfast_backmult * a->fast_backaverage;
	a->hang_backaverage = a->hang_backmult * a->abs_out_sample + a->onemhang_backmult * a->hang_backaverage;

	if ((a->abs_out_sample >= a->ring_max) && (a->abs_out_sample > 0.0))
	{
		a->ring_max = 0.0;
		k = a->out_index;
		for (j = 0; j < a->attack_buffsize; j++)
		{
			if (++k == a->ring_buffsize)
				k = 0;
			if (a->abs_ring[k] > a->ring_max)
				a->ring_max = a->abs_ring[k];
		}
	}
	if (a->abs_ring[a->in_index] > a->ring_max)
		a->ring_max = a->abs_ring[a->in_index];
}

static void wcpagc_state (WCPAGC a, double ring_max, double fast_backaverage, double hang_backaverage)
{
	if (a->hang_counter > 0)
		--a->hang_counter;

	switch (a->state)
	{
	case 0:
		{
			if (ring_max >= a->volts)
			{
				a->volts += (ring_max - a->volts) * a->attack_mult;
			}
			else
			{
				if (a->volts > a->pop_ratio * fast_backaverage)
				{
					a->state = 1;
					a->volts += (ring_max - a->volts) * a->fast_decay_mult;
				}
				else
				{
					if (a->hang_enable && (hang_backaverage > a->hang_level))
					{
						a->state = 2;
						a->hang_counter = (int)(a->hangtime * a->sample_rate);
						a->decay_type = 1;
					}
					else
					{
						a->state = 3;
						a->volts += (ring_max - a->volts) * a->decay_mult;
						a->decay_type = 0;
					}
				}
			}
			break;
		}
	case 1:
		{
			if (ring_max >= a->volts)
			{
				a->state = 0;
				a->volts += (ring_max - a->volts) * a->attack_mult;
			}
			else
			{
				if (a->volts > a->save_volts)
				{
					a->volts += (ring_max - a->volts) * a->fast_decay_mult;
				}
				else
				{
					if (a->hang_counter > 0)
					{
						a->state = 2;
					}
					else
					{
						if (a->decay_type == 0)
						{
							a->state = 3;
							a->volts += (ring_max - a->volts) * a->decay_mult;
						}
						else
						{
							a->state = 4;
							a->volts += (ring_max - a->volts) * a->hang_decay_mult;
						}
					}
				}
			}
			break;
		}
	case 2:
		{
			if (ring_max >= a->volts)
			{
				a->state = 0;
				a->save_volts = a->volts;
				a->volts += (ring_max - a->volts) * a->attack_mult;
			}
			else
			{
				if (a->hang_counter == 0)
				{
					a->state = 4;
					a->volts += (ring_max - a->volts) * a->hang_decay_mult;
				}
			}
			break;
		}
	case 3:
		{
			if (ring_max >= a->volts)
			{
				a->state = 0;
				a->save_volts = a->volts;
				a->volts += (ring_max - a->volts) * a->attack_mult;
			}
			else
			{
				a->volts += (ring_max - a->volts) * a->decay_mult;
			}
			break;
		}
	case 4:
		{
			if (ring_max >= a->volts)
			{
				a->state = 0;
				a->save_volts = a->volts;
				a->volts += (ring_max - a->volts) * a->attack_mult;
			}
			else
			{
				a->volts += (ring_max - a->volts) * a->hang_decay_mult;
			}
			break;
		}
	default:
		{
			a->state = 0;
		}
	}
}

static void wcpagc_output (WCPAGC a, int i)
{
	double mult;
	if (a->volts < a->min_volts)
		a->volts = a->min_volts;
	a->gain = a->volts * a->inv_out_target;
	mult = (a->out_target - a->slope_constant * min (0.0, log10(a->inv_max_input * a->volts))) / a->volts;
	a->out[2 * i + 0] = a->out_sample[0] * mult;
	a->out[2 * i + 1] = a->out_sample[1] * mult;
}

static void agc_run_alone (WCPAGC a)
{
	int i;
	if (a->run)
	{
		if (a->mode == 0)
		{
			for (i = 0; i < a->io_buffsize; i++)
			{
				a->out[2 * i + 0] = a->fixed_gain * a->in[2 * i + 0];
				a->out[2 * i + 1] = a->fixed_gain * a->in[2 * i + 1];
			}
			return;
		}

		for (i = 0; i < a->io_buffsize; i++)
		{
			wcpagc_detect (a, i);
			wcpagc_state (a, a->ring_max, a->fast_backaverage, a->hang_backaverage);
			wcpagc_output (a, i);
		}
	}
	else if (a->out != a->in)
		memcpy(a->out, a->in, a->io_buffsize * sizeof (complex));
}

//
// Two receive channels, the two ears, share one AGC gain.
//
// Each arm keeps its own look-ahead ring and its own detector. What is
// shared is the drive: the state machine runs ONCE per sample, on the
// LARGER of the two arms' peaks (ring_max) and of their two back-averages,
// and the one resulting gain is applied to both delayed samples. The
// stronger arm sets the gain, so it is held at the output target and does
// not clip; the weaker ear comes out lower by the difference.
//
// The channels run on two threads, so the pair meets here: whichever
// arrives first waits, whichever arrives second runs the whole block for
// both and lets the first go. Both buffers are valid at that moment, and
// both channels' results are ready when either returns.
//
// If the partner does not come within AGCPAIR_TIMEOUT_MS the waiter runs
// alone: a stalled or switched-off partner must not stall this channel for
// good. That costs the timeout on every block until SetRXAAGCLink(.., 0)
// takes the link away, so the caller removes the link before it stops
// feeding either channel.
//


static void xwcpagc_pair (WCPAGC a, WCPAGC b)
{
	int i;
	if (!a->run || !b->run || a->mode == 0 || b->mode == 0 || a->io_buffsize != b->io_buffsize)
	{
		agc_run_alone (a);
		agc_run_alone (b);
		return;
	}
	for (i = 0; i < a->io_buffsize; i++)
	{
		wcpagc_detect (a, i);
		wcpagc_detect (b, i);
		wcpagc_state (a, max(a->ring_max, b->ring_max),
		                 max(a->fast_backaverage, b->fast_backaverage),
		                 max(a->hang_backaverage, b->hang_backaverage));
		//
		// b follows a's state, so that taking the link away leaves both
		// where they were instead of one of them stale.
		//
		b->volts = a->volts;
		b->save_volts = a->save_volts;
		b->state = a->state;
		b->hang_counter = a->hang_counter;
		b->decay_type = a->decay_type;
		wcpagc_output (a, i);
		wcpagc_output (b, i);
	}
}

//
// Called from xwcpagc() for a linked channel. Returns 1 when the block has
// been done (by this thread or by the partner's), 0 when the caller is to
// run it alone because no partner came.
//
int xwcpagc_paired (WCPAGC a)
{
	agcpair *p = (agcpair *)a->pair;
	int slot = a->slot;
	//
	// A partner that is switched off, slewing down or bypassed will not come
	// to the rendezvous, and waiting for it costs AGCPAIR_TIMEOUT_MS on every
	// block - far more than a block lasts, so the receive path falls behind
	// and the input overflows. Look first, and run alone if it is not
	// running. (Still racy against a channel stopping this instant; that
	// costs one timeout, not one per block.)
	//
	{
		int o = p->chan[1 - slot];
		if (!_InterlockedAnd (&ch[o].exchange, 1) || _InterlockedAnd (&ch[o].iob.pd->exec_bypass, 1))
			return 0;
	}
	if (InterlockedIncrement (&p->arrived) == 1)
	{
		// first here: wait for the partner
		if (WaitForSingleObject (p->done[slot], AGCPAIR_TIMEOUT_MS) == WAIT_OBJECT_0)
			return 1;
		if (__sync_bool_compare_and_swap (&p->arrived, 1, 0))
			return 0;					// nobody came
		// the partner arrived at this moment and is running both
		WaitForSingleObject (p->done[slot], INFINITE);
		return 1;
	}
	// second here: run both
	xwcpagc_pair (p->m[0], p->m[1]);
	p->arrived = 0;
	ReleaseSemaphore (p->done[1 - slot], 1, 0);
	return 1;
}

//
// The link goes with either end.
//
void wcpagc_pair_detach (WCPAGC a)
{
	if (a->pair != NULL)
	{
		agcpair *p = (agcpair *)a->pair;
		WCPAGC o = p->m[1 - a->slot];
		if (o != NULL) o->pair = NULL;
		p->m[0] = NULL;
		p->m[1] = NULL;
		a->pair = NULL;
	}
}

//
// Link the AGCs of two receive channels (the diversity ear split) so they
// share one gain, or take the link away. Unlinked is the ordinary AGC.
//
// Both channels' csDSP are held, so neither is inside xwcpagc() while the
// pointers change. A channel waiting there for a partner holds its csDSP,
// so this may wait up to AGCPAIR_TIMEOUT_MS for it.
//
PORT void
SetRXAAGCLink (int channel_a, int channel_b, int on)
{
	WCPAGC a, b;
	if (channel_a == channel_b) return;
	EnterCriticalSection (&ch[channel_a].csDSP);
	EnterCriticalSection (&ch[channel_b].csDSP);
	a = rxa[channel_a].agc.p;
	b = rxa[channel_b].agc.p;
	if (on)
	{
		if (agc_pair.done[0] == NULL)
		{
			agc_pair.done[0] = CreateSemaphore (0, 0, 1000, 0);
			agc_pair.done[1] = CreateSemaphore (0, 0, 1000, 0);
		}
		agc_pair.arrived = 0;
		agc_pair.m[0] = a;
		agc_pair.m[1] = b;
		agc_pair.chan[0] = channel_a;
		agc_pair.chan[1] = channel_b;
		a->slot = 0;
		b->slot = 1;
		// b starts from a's state, as it does after every paired sample
		b->volts = a->volts;
		b->save_volts = a->save_volts;
		b->state = a->state;
		b->hang_counter = a->hang_counter;
		b->decay_type = a->decay_type;
		a->pair = &agc_pair;
		b->pair = &agc_pair;
	}
	else
	{
		a->pair = NULL;
		b->pair = NULL;
		agc_pair.m[0] = NULL;
		agc_pair.m[1] = NULL;
	}
	LeaveCriticalSection (&ch[channel_b].csDSP);
	LeaveCriticalSection (&ch[channel_a].csDSP);
}

