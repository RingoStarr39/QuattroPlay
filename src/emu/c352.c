/*
    C352 chip emulator for QuattroPlay
    written by ctr with help from MAME source code (written by R.Belmont)

    perhaps internal registers are readable at 0x100-0x1ff
*/
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "c352.h"
#include "../lib/vgm.h"

int C352_init(C352 *c, uint32_t clk)
{
    c->mute_mask=0;
    c->rate = clk/288;

    memset(c->v,0,sizeof(C352_Voice)*C352_VOICES);
    memset(c->out,0,4*sizeof(int16_t));

    c->control1 = 0;
    c->control2 = 0;
    c->random = 0x1234;
    return c->rate;
}

uint16_t C352RegMap[8] = {
    offsetof(C352_Voice,vol_f),
    offsetof(C352_Voice,vol_r),
    offsetof(C352_Voice,freq),
    offsetof(C352_Voice,flags),
    offsetof(C352_Voice,wave_bank),
    offsetof(C352_Voice,wave_start),
    offsetof(C352_Voice,wave_end),
    offsetof(C352_Voice,wave_loop),
};

void C352_write(C352 *c, uint16_t addr, uint16_t data)
{
    if(c->vgm_log)
        vgm_write(0xe1,0,addr,data);

    int i;

    if(addr < 0x100)
        *(uint16_t*)((void*)&c->v[addr/8]+C352RegMap[addr%8]) = data;
    else if(addr == 0x200)
        c->control1 = data;
    else if(addr == 0x201)
        c->control2 = data;
    else if(addr == 0x202) // execute keyons/keyoffs
    {
        for(i=0;i<C352_VOICES;i++)
        {
            if((c->v[i].flags & C352_FLG_KEYON))
            {
                c->v[i].pos = (c->v[i].wave_bank<<16) | c->v[i].wave_start;

				c->v[i].counter = 0xffff;

                c->v[i].flags |= C352_FLG_BUSY;
                c->v[i].flags &= ~(C352_FLG_KEYON|C352_FLG_LOOPHIST);

                c->v[i].latch_flags = c->v[i].flags;

                c->v[i].curr_vol[0] = c->v[i].curr_vol[1] = 0;
                c->v[i].curr_vol[2] = c->v[i].curr_vol[3] = 0;
            }
            else if(c->v[i].flags & C352_FLG_KEYOFF)
            {
                c->v[i].flags &= ~(C352_FLG_BUSY|C352_FLG_KEYOFF);
                c->v[i].counter = 0xffff;
            }
        }
    }
}

uint16_t C352_read(C352 *c, uint16_t addr)
{
    if(addr < 0x100)
        return *(uint16_t*)((void*)&c->v[addr/8]+C352RegMap[addr%8]);
    else
        return 0;
}


void C352_fetch_sample(C352 *c, int i)
{
    C352_Voice *v = &c->v[i];
	v->last_sample = v->sample;

    if(~v->flags & C352_FLG_BUSY)
    {
        v->sample = 0;
    }
    else if(v->flags & C352_FLG_NOISE)
    {
        c->random = (c->random>>1) ^ ((-(c->random&1)) & 0xfff6);
        v->sample = c->random;
	}
	else
	{
		int8_t s, s1,s2;

		s = (int8_t)c->wave[v->pos&c->wave_mask];

		if(v->flags & C352_FLG_MULAW)
        {
            // rom encoding differs a bit
            if(c->mulaw_type == C352_MULAW_TYPE_C140)
            {
                // C140 mulaw (based on MAME for now)
                s1=s&7;
                s2=s>>3;
                v->sample = (s2<<s1)<<3;
                if (s1 && s2<0)
                    v->sample -= ((2<<(s1-1))-1)<<7;
                else if (s1)
                    v->sample += ((2<<(s1-1))-1)<<7;
            }
            else
            {
                // C352 mulaw
                s2 = (s&0x7f)>>4;
                s1=s;

                v->sample = ((s2*s2)<<4) - (~(s2<<1))*(s1&0x0f);
                v->sample = (s&0x80) ? (~v->sample)<<5 : v->sample<<5;
            }
        }
		else
        {
			v->sample = s<<8;
        }

		uint16_t pos = v->pos&0xffff;

        if((v->flags & C352_FLG_LOOP) && v->flags & C352_FLG_REVERSE)
        {
			// backwards>forwards
            if((v->flags & C352_FLG_LDIR) && pos == v->wave_loop)
                v->flags &= ~C352_FLG_LDIR;
			// forwards>backwards
            else if(!(v->flags & C352_FLG_LDIR) && pos == v->wave_end)
                v->flags |= C352_FLG_LDIR;

			v->pos += (v->flags&C352_FLG_LDIR) ? -1 : 1;
        }
        else if(pos == v->wave_end)
        {
            if((v->flags & C352_FLG_LINK) && (v->flags & C352_FLG_LOOP))
            {
                v->pos = (v->wave_start<<16) | v->wave_loop;
                v->flags |= C352_FLG_LOOPHIST;
            }
            else if(v->flags & C352_FLG_LOOP)
            {
                v->pos = (v->pos&0xff0000) | v->wave_loop;
                v->flags |= C352_FLG_LOOPHIST;
            }
            else
            {
                v->flags |= C352_FLG_KEYOFF;
                v->flags &= ~C352_FLG_BUSY;
            }
        }
		else
		{
			v->pos += (v->flags&C352_FLG_REVERSE) ? -1 : 1;
		}
	}
}


void C352_update_volume(C352 *c,int i,int ch,uint8_t v)
{
    // disabling filter also disables volume ramp?
    if(c->v[i].latch_flags & C352_FLG_FILTER)
        c->v[i].curr_vol[ch] = v;

    // do volume ramping to prevent clicks
    int16_t vol_delta = c->v[i].curr_vol[ch] - v;
    if(vol_delta != 0)
        c->v[i].curr_vol[ch] += (vol_delta>0) ? -1 : 1;
}

int16_t C352_update_voice(C352 *c, int i)
{
    C352_Voice *v = &c->v[i];

    uint32_t next_counter = v->counter + v->freq;

	if(next_counter & 0x10000)
        C352_fetch_sample(c,i);

	if((next_counter^v->counter) & 0x18000)
    {
        C352_update_volume(c,i,0,c->v[i].vol_f>>8);
        C352_update_volume(c,i,1,c->v[i].vol_f&0xff);
        C352_update_volume(c,i,2,c->v[i].vol_r>>8);
        C352_update_volume(c,i,3,c->v[i].vol_r&0xff);
    }

	v->counter = next_counter&0xffff;

	int32_t temp = v->sample;
	// Interpolate samples
    if((v->latch_flags & C352_FLG_FILTER) == 0)
         temp = v->last_sample + (v->counter*(v->sample-v->last_sample)>>16);

    return temp;
}

void C352_update(C352 *c)
{
    int i;
    int16_t s;
    uint16_t flags;

    c->out[0]=c->out[1]=c->out[2]=c->out[3]=0;

    for(i=0;i<C352_VOICES;i++)
    {
        s = C352_update_voice(c,i);

        if(!(c->mute_mask & 1<<i))
        {
            flags = c->v[i].latch_flags;

            // Left
            c->out[0] += (flags & C352_FLG_PHASEFL) ? -s * (c->v[i].curr_vol[0])
                                                    :  s * (c->v[i].curr_vol[0]);
            c->out[2] += (flags & C352_FLG_PHASERL) ? -s * (c->v[i].curr_vol[2])
                                                    :  s * (c->v[i].curr_vol[2]);

            // Right
            c->out[1] += (flags & C352_FLG_PHASEFR) ? -s * (c->v[i].curr_vol[1])
                                                    :  s * (c->v[i].curr_vol[1]);
            c->out[3] += (flags & C352_FLG_PHASEFR) ? -s * (c->v[i].curr_vol[3])
                                                    :  s * (c->v[i].curr_vol[3]);
        }
    }
}
