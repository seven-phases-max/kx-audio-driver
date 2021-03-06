// kX WDM Audio Driver
// Copyright (c) Eugene Gavrilov, 2001-2014.
// All rights reserved

/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */


#include "common.h"

#include "interface/guids.h"

#include "eax/eax10.h"
#include "eax/eax20.h"
#include "eax/eax30.h"
#include "eax/3dl2.h"

#include "wdm/tbl_wave.h"

#pragma code_seg("PAGE")
NTSTATUS create_wave
(
    OUT     PUNKNOWN *  Unknown,
    IN      REFCLSID,
    IN      PUNKNOWN    UnknownOuter    OPTIONAL,
    IN      POOL_TYPE   PoolType
)
{
    PAGED_CODE();

    ASSERT(Unknown);

    STD_CREATE_BODY_(CMiniportWave,Unknown,UnknownOuter,PoolType,PMINIPORTWAVECYCLIC);
}

#pragma code_seg("PAGE")
STDMETHODIMP CMiniportWave::NonDelegatingQueryInterface(IN      REFIID  Interface,
								 OUT     PVOID * Object)
{
    PAGED_CODE();

    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface,IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVECYCLIC(this)));
    }
    else
    if (IsEqualGUIDAligned(Interface,IID_IMiniport))
    {
        *Object = PVOID(PMINIPORT(this));
    }
    else
    if (IsEqualGUIDAligned(Interface,IID_IMiniportWaveCyclic))
    {
        *Object = PVOID(PMINIPORTWAVECYCLIC(this));
    }
    else
    if (IsEqualGUIDAligned(Interface,IID_IPowerNotify))
    {
        *Object = PVOID(PPOWERNOTIFY(this));
    }
    else
    if (IsEqualGUIDAligned(Interface,IID_IPinCount))
    {
    	*Object=PVOID(PPINCOUNT(this));
    }
    else
    if (IsEqualGUIDAligned(Interface,IID_IMiniportDMus))
    {
    	*Object=NULL; // FIXME
    }
    else
    {
    	debug(DWDM,"!!! - (Wave) try to delegate to ref_iid unknown\n");
    	debug(DWDM,"!!! -- refid:"
    	"%x.%x.%x-%x-%x-%x-%x-%x-%x-%x-%x\n",
    	  Interface.Data1,
          Interface.Data2,
          Interface.Data3,
          Interface.Data4[0],
          Interface.Data4[1],
          Interface.Data4[2],
          Interface.Data4[3],
          Interface.Data4[4],
          Interface.Data4[5],
          Interface.Data4[6],
          Interface.Data4[7]);

        *Object = NULL;
    }

    if (*Object)
    {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

#pragma code_seg("PAGE")
CMiniportWave::~CMiniportWave(void)
{
    PAGED_CODE();

    debug(DWDM,"[CMiniportWave::~CMiniportWave]\n");
    if(magic!=WAVE_MAGIC)
    {
    	debug(DWDM," !!! [~CMiniportWave]: magic (%x) != %x\n",magic,WAVE_MAGIC);
    	magic=0;
    	return;
    }
    magic=0;

    if (Port)
    {
        Port->Release();
        Port = NULL;
    }
    if (AdapterCommon)
    {
    	for(int i=0;i<MAX_WAVE_DEVICES;i++)
    	{
    	 if(((CAdapterCommon *)AdapterCommon)->Wave[i]==this)
    	 {
    	  ((CAdapterCommon *)AdapterCommon)->Wave[i]=NULL;
    	  break;
    	 }
    	}
        AdapterCommon->Release();
        AdapterCommon = NULL;
    }
}

#pragma code_seg("PAGE")
STDMETHODIMP CMiniportWave::Init(IN      PUNKNOWN        UnknownAdapter,
				    IN      PRESOURCELIST   ResourceList,
                                    IN      PPORTWAVECYCLIC Port_)
{
    PAGED_CODE();

    ASSERT(UnknownAdapter);
    ASSERT(ResourceList);
    ASSERT(Port_);

    debug(DWDM,"[CMiniportWave::init]\n");

    magic=WAVE_MAGIC;
    kx3d_compat=0;
    kx3d_sp8ps=0;

    listener.instance=this;

    PowerState=PowerDeviceD0;

    AdapterCommon=NULL;
    Port=NULL;

    //
    // AddRef() is required because we are keeping this pointer.
    //
    Port = Port_;
    Port->AddRef();

    //
    // We want the IAdapterCommon interface on the adapter common object,
    // which is given to us as a IUnknown.  The QueryInterface call gives us
    // an AddRefed pointer to the interface we want.
    //
    NTSTATUS ntStatus =
        UnknownAdapter->QueryInterface
        (
            IID_IAdapterCommon,
            (PVOID *) &AdapterCommon
        );

    if( !NT_SUCCESS(ntStatus) )
    {
        // clean up AdapterCommon
        if( AdapterCommon )
        {
            AdapterCommon->Release();
            AdapterCommon = NULL;
        }

        // release the port
        Port->Release();
        Port = NULL;
    } else 
    {
     for(int i=0;i<MAX_WAVE_DEVICES;i++)
     {
      if(((CAdapterCommon*)AdapterCommon)->Wave[i]==0)
      {
       ((CAdapterCommon*)AdapterCommon)->Wave[i]=this;
       wave_number=i;
       break;
      }
     }
     hw=((CAdapterCommon *)AdapterCommon)->hw;

     kx_spin_lock_init(hw,&listener_lock,"listener");
    }

    if(!NT_SUCCESS(ntStatus))
     debug(DWDM,"!!! Init failed\n");

    return ntStatus;
}

#pragma code_seg("PAGE")
STDMETHODIMP CMiniportWave::GetDescription(OUT     PPCFILTER_DESCRIPTOR *  OutFilterDescriptor)
{
    PAGED_CODE();

    ASSERT(OutFilterDescriptor);

    switch(wave_number)
    {
    	case 0:
                // limit support to basic formats [recording] (16/48)
                if(((CAdapterCommon *)AdapterCommon)->is_vista)
                {
                  MiniportPinsHiFi[WAVE_NODE_WAVE_IN].KsPinDescriptor.DataRangesCount=REC_FORMATS_VISTA_LIMIT;
                  // support also 96 and 44.1 for direct spdif recording

                  MiniportPinsHiFi[WAVE_NODE_WAVE_OUT].KsPinDescriptor.DataRangesCount=SIZEOF_ARRAY(PinDataRangePointersPlaybackMultichannelOnly);
                  MiniportPinsHiFi[WAVE_NODE_WAVE_OUT].KsPinDescriptor.DataRanges=PinDataRangePointersPlaybackMultichannelOnly;
                  // this assumes: PinDataRangesStreamPlaybackMultichannel @48000 only, 16-32 bit
                }
                else
                {
                  // for AC-3 under XP, disable 16/48 PCM support on AC-3 pin [3541]
                  MiniportPinsHiFi[WAVE_NODE_SPDIF].KsPinDescriptor.DataRangesCount=2;

                  // default WinXP values:
                  // MiniportPinsHiFi[WAVE_NODE_WAVE_OUT].KsPinDescriptor.DataRangesCount=SIZEOF_ARRAY(PinDataRangePointersPlaybackHiFi);
                  // MiniportPinsHiFi[WAVE_NODE_WAVE_OUT].KsPinDescriptor.DataRanges=PinDataRangePointersPlaybackHiFi;
                  // this assumes: PinDataRangesStreamPlaybackMin, PinDataRangesStreamPlaybackHiFi [16-32, any SR]
                }

                // 3548
                if(!hw->is_10k2 /*&& ((CAdapterCommon *)AdapterCommon)->is_vista*/) // 10k1: limit 16-bit only for 'master mixer' etc.
                {
                  PinDataRangesStreamPlaybackMultichannel[0].MaximumBitsPerSample=16;
                  PinDataRangesStreamPlaybackMultichannel[1].MaximumBitsPerSample=16;
                }
                
    		    *OutFilterDescriptor = &MiniportFilterDescriptorHiFi;
    		    break;
    	case 1:
    		    MiniportNodes[WAVE_SUPERMIX].Name=&TOPO_WAVEOUT23_NAME;
                MiniportNodes[WAVE_VOLUME1].Name=&TOPO_WAVEOUT23_VOLUME_NAME;
                MiniportNodes[WAVE_VOLUME2].Name=&TOPO_WAVEOUT23_VOLUME_NAME;
                MiniportNodes[WAVE_SUM].Name=&TOPO_WAVEOUT23_NAME;

                // limit support to basic formats 16/48
                if(((CAdapterCommon *)AdapterCommon)->is_vista)
                {
                  MiniportPins23[0].KsPinDescriptor.DataRangesCount=SIZEOF_ARRAY(PinDataRangePointersPlaybackMinimumOnly);
                  MiniportPins23[0].KsPinDescriptor.DataRanges=PinDataRangePointersPlaybackMinimumOnly;
                }

                *OutFilterDescriptor = &MiniportFilterDescriptor23;

                break;
    	case 2:
    		    MiniportNodes[WAVE_SUPERMIX].Name=&TOPO_WAVEOUT45_NAME;
                MiniportNodes[WAVE_VOLUME1].Name=&TOPO_WAVEOUT45_VOLUME_NAME;
                MiniportNodes[WAVE_VOLUME2].Name=&TOPO_WAVEOUT45_VOLUME_NAME;
                MiniportNodes[WAVE_SUM].Name=&TOPO_WAVEOUT45_NAME;

                // limit support to basic formats 16/48
                if(((CAdapterCommon *)AdapterCommon)->is_vista)
                {
                  MiniportPins45[0].KsPinDescriptor.DataRangesCount=SIZEOF_ARRAY(PinDataRangePointersPlaybackMinimumOnly);
                  MiniportPins45[0].KsPinDescriptor.DataRanges=PinDataRangePointersPlaybackMinimumOnly;
                }

                *OutFilterDescriptor = &MiniportFilterDescriptor45;
    		    break;
    	case 3:
    		    MiniportNodes[WAVE_SUPERMIX].Name=&TOPO_WAVEOUT67_NAME;
                MiniportNodes[WAVE_VOLUME1].Name=&TOPO_WAVEOUT67_VOLUME_NAME;
                MiniportNodes[WAVE_VOLUME2].Name=&TOPO_WAVEOUT67_VOLUME_NAME;
                MiniportNodes[WAVE_SUM].Name=&TOPO_WAVEOUT67_NAME;

                // limit support to basic formats 16/48
                if(((CAdapterCommon *)AdapterCommon)->is_vista)
                {
                  MiniportPins67[0].KsPinDescriptor.DataRangesCount=SIZEOF_ARRAY(PinDataRangePointersPlaybackMinimumOnly);
                  MiniportPins67[0].KsPinDescriptor.DataRanges=PinDataRangePointersPlaybackMinimumOnly;
                }

                *OutFilterDescriptor = &MiniportFilterDescriptor67;
    		    break;
    }
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP CMiniportWave::DataRangeIntersection
(
    IN      ULONG           PinId,
    IN      PKSDATARANGE    ClientDataRange,
    IN      PKSDATARANGE    MyDataRange,
    IN      ULONG           OutputBufferLength,
    OUT     PVOID           ResultantFormat,
    OUT     PULONG          ResultantFormatLength
)
{
   PAGED_CODE();

    if (!IsEqualGUIDAligned(ClientDataRange->MajorFormat,
           KSDATAFORMAT_TYPE_AUDIO) && 
        !IsEqualGUIDAligned(ClientDataRange->MajorFormat, 
           KSDATAFORMAT_TYPE_WILDCARD))
    {
    	debug(DWDM,"!!! -- DataIntersection:: it was not AUDIO and not *\n");
        return STATUS_NO_MATCH;
    }

    // Major format is: audio or *

    if (!IsEqualGUIDAligned(ClientDataRange->SubFormat,
           KSDATAFORMAT_SUBTYPE_DOLBY_AC3_SPDIF) &&
        !IsEqualGUIDAligned(ClientDataRange->SubFormat, 
           KSDATAFORMAT_TYPE_WILDCARD)
           )
    {
    	// subformat is: not ac3 and not *
    	if(PinId==WAVE_NODE_SPDIF)
    	{
    		debug(DWDM,"!!! -- DataIntersection: node=spdif, format!=ac3\n");
    		return STATUS_NO_MATCH;
    	}

    	if(!IsEqualGUIDAligned(ClientDataRange->SubFormat,
    	    KSDATAFORMAT_SUBTYPE_PCM))
    	    {
    	debug(DWDM,"!!! -- DataIntersection (not PCM, not AC3, not *) unknown subformat:"
    	"%x.%x.%x-%x-%x-%x-%x-%x-%x-%x-%x\n",
    	  ClientDataRange->SubFormat.Data1,
          ClientDataRange->SubFormat.Data2,
          ClientDataRange->SubFormat.Data3,
          ClientDataRange->SubFormat.Data4[0],
          ClientDataRange->SubFormat.Data4[1],
          ClientDataRange->SubFormat.Data4[2],
          ClientDataRange->SubFormat.Data4[3],
          ClientDataRange->SubFormat.Data4[4],
          ClientDataRange->SubFormat.Data4[5],
          ClientDataRange->SubFormat.Data4[6],
          ClientDataRange->SubFormat.Data4[7]);

          return STATUS_NO_MATCH;
            }
            else
            {
            	   int sz=0;

            	   // should be PCM
    		   if(IsEqualGUIDAligned(ClientDataRange->Specifier,
    	    		KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
    	    	   {
    	    	    sz=sizeof(WAVEFORMATEXTENSIBLE);
    	    	   }
    	    	   else
   		    if(IsEqualGUIDAligned(ClientDataRange->Specifier,
    	    		KSDATAFORMAT_SPECIFIER_DSOUND))

    	    	    {
    	    	     sz=sizeof(WAVEFORMATEXTENSIBLE)+2*sizeof(ULONG);
    	    	     debug(DWDM,"!!! btw, DataRangeIntersection: specifier is DSound!\n");
    	    	    }
    	    	    else
    	    	    {
    	    	     debug(DWDM,"!!! DataRangeIntersection: unknown specifier!\n");
    	    	     return STATUS_NO_MATCH;
    	    	    }


                   if (!OutputBufferLength) 
                   {
                       *ResultantFormatLength = sizeof(KSDATAFORMAT) + sz;
                       return STATUS_BUFFER_OVERFLOW;
                   } 
                   
                   if (OutputBufferLength < (sizeof(KSDATAFORMAT) + sz)) 
                   {
                       debug(DWDM,"!! buffer was < than needed\n");
                       return STATUS_BUFFER_TOO_SMALL;
                   }

                   // Fill in the structure the datarange structure.
                   //
                   RtlCopyMemory(ResultantFormat, MyDataRange, sizeof(KSDATAFORMAT));

                   // Modify the size of the data format structure to fit the WAVEFORMATEXTENSIBLE
                   // structure.
                   //
                   ((PKSDATAFORMAT)ResultantFormat)->FormatSize =
                       sizeof(KSDATAFORMAT) + sz;

                   // Append the WAVEFORMATEXTENSIBLE structure
                   //
                   PWAVEFORMATEXTENSIBLE pWfxExt = 
                       (PWAVEFORMATEXTENSIBLE)((PKSDATAFORMAT)ResultantFormat + 1);

   		    if(IsEqualGUIDAligned(ClientDataRange->Specifier,
    	    		KSDATAFORMAT_SPECIFIER_DSOUND))
    	    	    {	
			((PKSDATAFORMAT_DSOUND)ResultantFormat)->BufferDesc.Flags = 0 ;
			((PKSDATAFORMAT_DSOUND)ResultantFormat)->BufferDesc.Control = 0 ;

			((PKSDATAFORMAT_DSOUND)ResultantFormat)->DataFormat.Specifier = 
          			KSDATAFORMAT_SPECIFIER_DSOUND;

			pWfxExt=(PWAVEFORMATEXTENSIBLE) &((PKSDATAFORMAT_DSOUND)ResultantFormat)->BufferDesc.WaveFormatEx;
    	    	    }	

#define my_min(a,b) ((a)<(b)?(a):(b))


        if(((CAdapterCommon *)AdapterCommon)->is_vista)
        {
            	  debug(DWDM,"Intersection: client: %dch/%dHz/%dBps vs my: %dch/%dHz/%dBps\n",
        		     (DWORD)((PKSDATARANGE_AUDIO) ClientDataRange)->MaximumChannels,
        		     (DWORD)((PKSDATARANGE_AUDIO) ClientDataRange)->MaximumSampleFrequency,
        		     (DWORD)((PKSDATARANGE_AUDIO) ClientDataRange)->MaximumBitsPerSample,

        		     (DWORD)((PKSDATARANGE_AUDIO) MyDataRange)->MaximumChannels,
        		     (DWORD)((PKSDATARANGE_AUDIO) MyDataRange)->MaximumSampleFrequency,
        		     (DWORD)((PKSDATARANGE_AUDIO) MyDataRange)->MaximumBitsPerSample);


                   pWfxExt->Format.nChannels = (WORD)((PKSDATARANGE_AUDIO) MyDataRange)->MaximumChannels;
                   pWfxExt->Format.nSamplesPerSec = ((PKSDATARANGE_AUDIO) MyDataRange)->MaximumSampleFrequency;
                   pWfxExt->Format.wBitsPerSample = (WORD)((PKSDATARANGE_AUDIO) MyDataRange)->MaximumBitsPerSample;
        }
        else
        {
                   pWfxExt->Format.nChannels = my_min(
                       (WORD)((PKSDATARANGE_AUDIO) ClientDataRange)->MaximumChannels,
                       (WORD)((PKSDATARANGE_AUDIO) MyDataRange)->MaximumChannels);

                   pWfxExt->Format.nSamplesPerSec = my_min(
                       ((PKSDATARANGE_AUDIO) ClientDataRange)->MaximumSampleFrequency,
                       ((PKSDATARANGE_AUDIO) MyDataRange)->MaximumSampleFrequency);

                   pWfxExt->Format.wBitsPerSample = my_min(
                   	(WORD)((PKSDATARANGE_AUDIO) ClientDataRange)->MaximumBitsPerSample,
                   	(WORD)((PKSDATARANGE_AUDIO) MyDataRange)->MaximumBitsPerSample);
        }

                   if(pWfxExt->Format.nSamplesPerSec < 
                       ((PKSDATARANGE_AUDIO) ClientDataRange)->MinimumSampleFrequency)
                       {
                         debug(DWDM,"!! nSamplePerSec: %d\n",pWfxExt->Format.nSamplesPerSec);
                         return STATUS_NO_MATCH;
                       }
                   if(pWfxExt->Format.nSamplesPerSec < 
                       ((PKSDATARANGE_AUDIO) MyDataRange)->MinimumSampleFrequency)
                       {
                         debug(DWDM,"!! nSamplePerSec: %d\n",pWfxExt->Format.nSamplesPerSec);
                         return STATUS_NO_MATCH;
                       }

                   if(pWfxExt->Format.wBitsPerSample < 
                       ((PKSDATARANGE_AUDIO) ClientDataRange)->MinimumBitsPerSample)
                       {
                         debug(DWDM,"!! nBitsPerSample: %d\n",pWfxExt->Format.wBitsPerSample);
                         return STATUS_NO_MATCH;
                       }
                   if(pWfxExt->Format.wBitsPerSample < 
                       ((PKSDATARANGE_AUDIO) MyDataRange)->MinimumBitsPerSample)
                       {
                         debug(DWDM,"!! nBitsPerSample: %d\n",pWfxExt->Format.wBitsPerSample);
                         return STATUS_NO_MATCH;
                       }

                   pWfxExt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;

                   pWfxExt->Format.nBlockAlign = 
                       (pWfxExt->Format.wBitsPerSample * pWfxExt->Format.nChannels) / 8;
                   pWfxExt->Format.nAvgBytesPerSec = 
                       pWfxExt->Format.nSamplesPerSec * pWfxExt->Format.nBlockAlign;
                   pWfxExt->Format.cbSize = 22;

                   pWfxExt->Samples.wValidBitsPerSample = pWfxExt->Format.wBitsPerSample;

                   pWfxExt->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

                   // This should be set to wave port's channel config

                   switch(pWfxExt->Format.nChannels)
                   {
                    default:
                    case 5+1:
                       pWfxExt->dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
                       break;
                    case 2:
                       pWfxExt->dwChannelMask = KSAUDIO_SPEAKER_STEREO;
                       break;
                    case 1:
                       pWfxExt->dwChannelMask = KSAUDIO_SPEAKER_MONO;
                       break;
                    case 5:
                       pWfxExt->dwChannelMask = KSAUDIO_SPEAKER_SURROUND|SPEAKER_LOW_FREQUENCY;
                       break;
                    case 4:
                       pWfxExt->dwChannelMask = KSAUDIO_SPEAKER_QUAD;
                       break;
                    case 6+1:
                       pWfxExt->dwChannelMask = KSAUDIO_SPEAKER_5POINT1|SPEAKER_BACK_CENTER;
                       break;
                    case 7+1:
                       pWfxExt->dwChannelMask = KSAUDIO_SPEAKER_7POINT1;
                       break;
                    case 3:
                       pWfxExt->dwChannelMask = SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER;
                       break;
                   }

                   // Now overwrite also the sample size in the ksdataformat structure.
                   // per XP ddk: this field is ignored
                   ((PKSDATAFORMAT)ResultantFormat)->SampleSize = pWfxExt->Format.nBlockAlign;
                   
                   // That we will return.
                   //
                   *ResultantFormatLength = sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE);

                   return STATUS_SUCCESS;
            }
    }
    // subformat is ac3 or *

    if(PinId!=WAVE_NODE_SPDIF)
    {
    	debug(DWDM,"!!! -- DataIntersection: node!=spdif, format==ac3\n");
    	return STATUS_NO_MATCH;
    }

    if (IsEqualGUIDAligned(ClientDataRange->Specifier, 
          KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) ||
        IsEqualGUIDAligned(ClientDataRange->Specifier, 
          KSDATAFORMAT_TYPE_WILDCARD))
    {
    	// waveformatex or *
        *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    }
    else if (IsEqualGUIDAligned(ClientDataRange->Specifier, 
               KSDATAFORMAT_SPECIFIER_DSOUND))
    {
    	// dsound
        *ResultantFormatLength = sizeof(KSDATAFORMAT_DSOUND);
    }
    else
    {
    	debug(DWDM,"!!! -- wave::intersection: unknown specifier\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    // Validate return buffer size. If the request is only for 
    // the size of the resultant structure, return it now.
    if (!OutputBufferLength) 
    {
    	debug(DWDM," -- wave::intersection: buffer overflow\n");
        return STATUS_BUFFER_OVERFLOW;
    } 
    else if (OutputBufferLength < *ResultantFormatLength)
    {
    	debug(DWDM," -- wave::intersection: buffer too small\n");
        return STATUS_BUFFER_TOO_SMALL;
    }

    // prepare AC3 format descriptor:
    
    PKSDATAFORMAT_WAVEFORMATEX  resultantFormatWFX;
    PWAVEFORMATEX  pWaveFormatEx;

    resultantFormatWFX = (PKSDATAFORMAT_WAVEFORMATEX) ResultantFormat;

    // Return the best (only) available format.
    // SampleSize must match nBlockAlign.
    resultantFormatWFX->DataFormat.FormatSize   = *ResultantFormatLength;
    resultantFormatWFX->DataFormat.Flags        = 0; // no special format flags
    resultantFormatWFX->DataFormat.SampleSize   = 4; // 2, 2 bytes/sample 
    resultantFormatWFX->DataFormat.Reserved     = 0; // always leave this zero

    resultantFormatWFX->DataFormat.MajorFormat  = KSDATAFORMAT_TYPE_AUDIO;
    INIT_WAVEFORMATEX_GUID(&resultantFormatWFX->DataFormat.SubFormat,
      WAVE_FORMAT_DOLBY_AC3_SPDIF );

    // Extra space for the (larger) DirectSound specifier
    if (IsEqualGUIDAligned(ClientDataRange->Specifier, 
          KSDATAFORMAT_SPECIFIER_DSOUND))
    {
        PKSDATAFORMAT_DSOUND resultantFormatDSound;
        resultantFormatDSound = (PKSDATAFORMAT_DSOUND)ResultantFormat;

        resultantFormatDSound->DataFormat.Specifier = 
          KSDATAFORMAT_SPECIFIER_DSOUND;
        
        // DirectSound format capabilities are not expressed 
        // this way in KS, so we express no capabilities. 
        // FIXME
        resultantFormatDSound->BufferDesc.Flags = 0 ;
        resultantFormatDSound->BufferDesc.Control = 0 ;

        pWaveFormatEx = &resultantFormatDSound->BufferDesc.WaveFormatEx;
    }
    else  // WAVEFORMATEX or WILDCARD (WAVEFORMATEX)
    {
        resultantFormatWFX->DataFormat.Specifier = 
          KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

        pWaveFormatEx = (PWAVEFORMATEX)((PKSDATAFORMAT) resultantFormatWFX + 1);
    }

    pWaveFormatEx->wFormatTag      = WAVE_FORMAT_DOLBY_AC3_SPDIF;     
    pWaveFormatEx->nChannels       = 2;     // stereo stream
    pWaveFormatEx->nSamplesPerSec  = 48000; // de facto standard for AC3-SPDIF
    pWaveFormatEx->wBitsPerSample  = 16;    // normal SPDIF sample size
    pWaveFormatEx->cbSize          = 0;     // no extra format info follows
    pWaveFormatEx->nBlockAlign     = 
      pWaveFormatEx->nChannels * pWaveFormatEx->wBitsPerSample / 8;
    pWaveFormatEx->nAvgBytesPerSec = 
      pWaveFormatEx->nSamplesPerSec * pWaveFormatEx->nBlockAlign;

    debug(DWDM,"!!! intersection: and now, AC-3 [native]!\n");
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP CMiniportWave::NewStream(OUT     PMINIPORTWAVECYCLICSTREAM * OutStream,
					   IN      PUNKNOWN                    OuterUnknown,
                                           IN      POOL_TYPE                   PoolType,
                                           IN      ULONG                       Pin,
                                           IN      BOOLEAN                     Capture,
                                           IN      PKSDATAFORMAT               DataFormat,
                                           OUT     PDMACHANNEL *               DmaChannel,
                                           OUT     PSERVICEGROUP *             ServiceGroup)
{
    PAGED_CODE();

    ASSERT(OutStream);
    ASSERT(DataFormat);
    ASSERT(DmaChannel);
    ASSERT(ServiceGroup);

    NTSTATUS ntStatus = STATUS_SUCCESS;

    CMiniportWaveStream *stream;

    if(Capture)
    {
    	// wavein
         stream = new (PoolType, 'swLA')
                             CMiniportWaveInStream(OuterUnknown);
    }
    else
    {
    	// waveout
    	stream = new (PoolType, 'swLA')
    			     CMiniportWaveOutStream(OuterUnknown);
    }
    if(stream)
    {
        stream->AddRef();
        ntStatus = stream->Init(this,PoolType,Pin,DataFormat,DmaChannel,ServiceGroup);
    }
    else
    {
      ntStatus=STATUS_INSUFFICIENT_RESOURCES;
    }

    if(NT_SUCCESS(ntStatus))
    {
    	*OutStream = PMINIPORTWAVECYCLICSTREAM(stream);
    	(*OutStream)->AddRef();
    	stream->Release();
    }
     else 
    { 
        if(stream)
        {
          stream->Release();
          stream=NULL;
        }
        *OutStream=NULL; 
        *DmaChannel=NULL;
        *ServiceGroup=NULL;
        return ntStatus; 
    }
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
NTSTATUS CMiniportWave::PropertyChannelConfig(IN PPCPROPERTY_REQUEST PropertyRequest)
{
 PAGED_CODE();

    ASSERT(PropertyRequest);

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;

    CMiniportWave *that =
        (CMiniportWave *) (PMINIPORTWAVECYCLIC)PropertyRequest->MajorTarget;

    if(!that)
    {
     debug(DWDM,"!!! Wave majortarget=0! ::chconfig\n");
     return STATUS_INVALID_PARAMETER;
    }

    if(that->magic!=WAVE_MAGIC)
    {
     debug(DWDM,"!!! Bad wave magic!\n");
     return STATUS_INVALID_PARAMETER;
    }

        if(PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
        {
            if(PropertyRequest->ValueSize >= sizeof(KSAUDIO_CHANNEL_CONFIG))
            {
            	// Force Surround
            	if(that && that->hw && that->hw->is_a2)
                  *(PLONG(PropertyRequest->Value)) = KSAUDIO_SPEAKER_7POINT1;
                else
                  *(PLONG(PropertyRequest->Value)) = KSAUDIO_SPEAKER_5POINT1;

                PropertyRequest->ValueSize = sizeof(KSAUDIO_CHANNEL_CONFIG);
                debug(DPROP,"Wave:chancfg: get\n");
                ntStatus = STATUS_SUCCESS;
            } else
            {
            	debug(DWDM,"!!! Wave:chancfg: get:: buffer too small\n");
                ntStatus = STATUS_BUFFER_TOO_SMALL;
            }
        } 
        else 
         if(PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
        {
            if(PropertyRequest->ValueSize >= (sizeof(KSPROPERTY_DESCRIPTION)))
            {
                // if return buffer can hold a KSPROPERTY_DESCRIPTION, return it
                PKSPROPERTY_DESCRIPTION PropDesc = PKSPROPERTY_DESCRIPTION(PropertyRequest->Value);

                PropDesc->AccessFlags       = KSPROPERTY_TYPE_BASICSUPPORT |
                                              KSPROPERTY_TYPE_GET |
                                              KSPROPERTY_TYPE_SET;
                PropDesc->DescriptionSize   = sizeof(KSPROPERTY_DESCRIPTION);
                PropDesc->PropTypeSet.Set   = KSPROPTYPESETID_General;
                PropDesc->PropTypeSet.Id    = VT_I4;
                PropDesc->PropTypeSet.Flags = 0;
                PropDesc->MembersListCount  = 0;
                PropDesc->Reserved          = 0;

                // set the return value size
                PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
                debug(DPROP,"Wave:chancfg: basic support 1\n");
                ntStatus = STATUS_SUCCESS;
            } else if(PropertyRequest->ValueSize >= sizeof(ULONG))
            {
                // if return buffer can hold a ULONG, return the access flags
                PULONG AccessFlags = PULONG(PropertyRequest->Value);
        
                *AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT |
                               KSPROPERTY_TYPE_GET |
                               KSPROPERTY_TYPE_SET;
        
                // set the return value size
                PropertyRequest->ValueSize = sizeof(ULONG);
                debug(DPROP,"Wave:chancfg: basic support 2\n");
                ntStatus = STATUS_SUCCESS;                    
            }
        }
        else
        if(PropertyRequest->Verb & KSPROPERTY_TYPE_SET)
        {
            if(PropertyRequest->ValueSize >= sizeof(KSAUDIO_CHANNEL_CONFIG))
            {
            	if((*(PLONG)PropertyRequest->Value)!=KSAUDIO_SPEAKER_5POINT1 && (*(PLONG)PropertyRequest->Value)!=KSAUDIO_SPEAKER_7POINT1 )
            	  debug(DWDM,"Wave:chancfg::set !!! Trying to SET ChannelConfig to %x [set in ControlPanel/Audio]\n",*(PLONG)(PropertyRequest->Value));
            	else
                  debug(DPROP,"Wave:chancfg:: set\n");
                ntStatus = STATUS_SUCCESS;
            } else
            {
            	debug(DWDM,"!!! Wave:chancfg: set:: buffer too small\n");
                ntStatus = STATUS_BUFFER_TOO_SMALL;
            }
        } else debug(DWDM,"!!! Unknown verb\n");

    return ntStatus;
}

#pragma code_seg("PAGE")
NTSTATUS CMiniportWave::PropertyCpuResources(IN PPCPROPERTY_REQUEST PropertyRequest)
{
 PAGED_CODE();

    ASSERT(PropertyRequest);

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;

        if(PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
        {
            if(PropertyRequest->ValueSize >= sizeof(LONG))
            {
                *(PLONG(PropertyRequest->Value)) = KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU;
                PropertyRequest->ValueSize = sizeof(LONG);
                ntStatus = STATUS_SUCCESS;
                debug(DPROP,"Wave:cpu: get (node=%d)\n",PropertyRequest->Node);
            } else
            {
            	debug(DWDM,"!!! -- wave::cpuresources: buffer too small\n");
                ntStatus = STATUS_BUFFER_TOO_SMALL;
            }
        } else if(PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
        {
            if(PropertyRequest->ValueSize >= (sizeof(KSPROPERTY_DESCRIPTION)))
            {
                // if return buffer can hold a KSPROPERTY_DESCRIPTION, return it
                PKSPROPERTY_DESCRIPTION PropDesc = PKSPROPERTY_DESCRIPTION(PropertyRequest->Value);

                PropDesc->AccessFlags       = KSPROPERTY_TYPE_BASICSUPPORT |
                                              KSPROPERTY_TYPE_GET;
                PropDesc->DescriptionSize   = sizeof(KSPROPERTY_DESCRIPTION);
                PropDesc->PropTypeSet.Set   = KSPROPTYPESETID_General;
                PropDesc->PropTypeSet.Id    = VT_I4;
                PropDesc->PropTypeSet.Flags = 0;
                PropDesc->MembersListCount  = 0;
                PropDesc->Reserved          = 0;

                // set the return value size
                PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
                ntStatus = STATUS_SUCCESS;
                debug(DPROP,"Wave:cpu: basic support 1\n");
            } else if(PropertyRequest->ValueSize >= sizeof(ULONG))
            {
                // if return buffer can hold a ULONG, return the access flags
                PULONG AccessFlags = PULONG(PropertyRequest->Value);
        
                *AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT |
                               KSPROPERTY_TYPE_GET |
                               KSPROPERTY_TYPE_SET;
        
                // set the return value size
                PropertyRequest->ValueSize = sizeof(ULONG);
                ntStatus = STATUS_SUCCESS;                    
                debug(DPROP,"Wave:cpu: basic support 2\n");
            }
        } else debug(DWDM,"!!! Unknown verb in wave::CpuResources\n");

    if(!NT_SUCCESS(ntStatus))
     debug(DWDM,"!!! Error CpuResources Property op\n");

    return ntStatus;
}

#pragma code_seg("PAGE")
NTSTATUS CMiniportWave::PropertyVolume(IN PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    CMiniportWaveStream *that =
        (CMiniportWaveStream *)(PMINIPORTWAVECYCLIC)PropertyRequest->MinorTarget;

    if(that)
    {
     if(that->magic==WAVEOUTSTREAM_MAGIC)
     {
      return CMiniportWaveOutStream::PropertyVolume(PropertyRequest);
     }
     else
     {
      debug(DWDM,"!!! Miniport::propertyVolume: magic!=waveoutstream_magic\n");
     }
    }
    else
    {
     return CMiniportWaveOutStream::PropertyVolume(PropertyRequest);;
    }
    return STATUS_INVALID_PARAMETER;
}

#pragma code_seg("PAGE")
NTSTATUS CMiniportWave::PropertyMixLevel(IN PPCPROPERTY_REQUEST PropertyRequest)
{
 PAGED_CODE();

 debug(DWDM,"!!! MixLevelProp verb: %x node: %d\n",
  PropertyRequest->Verb,PropertyRequest->Node);

 return STATUS_NOT_IMPLEMENTED;

/*
 int nch=MAX_TOPOLOGY_CHANNELS*MAX_TOPOLOGY_CHANNELS;

 if(PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
 {
      if(PropertyRequest->ValueSize==0)
      {
          PropertyRequest->ValueSize =  nch * sizeof(KSAUDIO_MIXLEVEL);
          return STATUS_BUFFER_OVERFLOW;
      } 
       else if(PropertyRequest->ValueSize >= nch * sizeof(KSAUDIO_MIXLEVEL))
      {
        PropertyRequest->ValueSize = nch * sizeof(KSAUDIO_MIXLEVEL);

        PKSAUDIO_MIXLEVEL MixLevel = (PKSAUDIO_MIXLEVEL)PropertyRequest->Value;

        for(int count = 0; count < nch; count++)
        {
            if((count%MAX_TOPOLOGY_CHANNELS)!=0)
            {
                MixLevel[count].Mute = TRUE;
                MixLevel[count].Level = 0;
            } else
            {
                MixLevel[count].Mute = FALSE;
                MixLevel[count].Level = KX_MAX_VOLUME;
            }
        }
        return STATUS_SUCCESS;
    }
 } else debug(DWDM,"Unsupported verb!\n");

 return STATUS_INVALID_PARAMETER;
 */
}


#pragma code_seg("PAGE")
NTSTATUS CMiniportWave::PropertyMixLevelCaps(IN PPCPROPERTY_REQUEST PropertyRequest)
{
 PAGED_CODE();

 debug(DWDM,"!!! PropMixLevCaps: verb: %x node: %d\n",
  PropertyRequest->Verb,PropertyRequest->Node);

 return STATUS_NOT_IMPLEMENTED; // FIXME: else works badly... :( but now is NEVER CALLED :)

/* if(PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
 {
      if(PropertyRequest->ValueSize==0)
      {
          PropertyRequest->ValueSize = 2 * sizeof(ULONG) + nch * sizeof(KSAUDIO_MIX_CAPS);
          return STATUS_BUFFER_OVERFLOW;
      } 
       else if(PropertyRequest->ValueSize == 2 * sizeof(ULONG))
      {
          PKSAUDIO_MIXCAP_TABLE MixCaps = (PKSAUDIO_MIXCAP_TABLE)PropertyRequest->Value;
          MixCaps->InputChannels = MAX_TOPOLOGY_CHANNELS;
          MixCaps->OutputChannels = MAX_TOPOLOGY_CHANNELS;
          return STATUS_BUFFER_OVERFLOW;
      } 
       else if(PropertyRequest->ValueSize == 2 * sizeof(ULONG) + nch * sizeof(KSAUDIO_MIX_CAPS))
      {
        debug(DWDM,"!!! mix: normal req\n");
        PropertyRequest->ValueSize = 2 * sizeof(ULONG) + nch * sizeof(KSAUDIO_MIX_CAPS);

        PKSAUDIO_MIXCAP_TABLE MixCaps = (PKSAUDIO_MIXCAP_TABLE)PropertyRequest->Value;
        MixCaps->InputChannels = MAX_TOPOLOGY_CHANNELS;
        MixCaps->OutputChannels = MAX_TOPOLOGY_CHANNELS;
        for(int count = 0; count < nch; count++)
        {
            if(!((count == 0) || (count == 3)))
            {
                MixCaps->Capabilities[count].Mute = TRUE;
                MixCaps->Capabilities[count].Minimum = 0;
                MixCaps->Capabilities[count].Maximum = 0;
                MixCaps->Capabilities[count].Reset = 0;
            } else
            {
                MixCaps->Capabilities[count].Mute = FALSE;
                MixCaps->Capabilities[count].Minimum = KX_MIN_VOLUME;
                MixCaps->Capabilities[count].Maximum = KX_MAX_VOLUME;
                MixCaps->Capabilities[count].Reset = KX_MAX_VOLUME;
            }
        }
        return STATUS_SUCCESS;
      }
      else
      {
        debug(DWDM,"!!! mix: unknown buff len =%d\n",PropertyRequest->ValueSize);
        return STATUS_INVALID_PARAMETER;
      }
 } else debug(DWDM,"!!! mix: Unsupported verb\n");
 return STATUS_INVALID_PARAMETER;
 */
}

#pragma code_seg("PAGE")
NTSTATUS CMiniportWave::PropertySpeakerGeometry(IN PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;

    CMiniportWave *that =
        (CMiniportWave *) (PMINIPORTWAVECYCLIC)PropertyRequest->MajorTarget;

    if(!that)
    {
     debug(DWDM,"!!! Wave majortarget=0! ::geometry\n");
     return STATUS_INVALID_PARAMETER;
    }

    if(that->magic!=WAVE_MAGIC)
    {
     debug(DWDM,"!!! Bad wave magic!\n");
     return STATUS_INVALID_PARAMETER;
    }

        if(PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
        {
            if(PropertyRequest->ValueSize >= sizeof(LONG))
            {
            	// Force 'Wide'
                *(PLONG(PropertyRequest->Value)) = KSAUDIO_STEREO_SPEAKER_GEOMETRY_WIDE;
                PropertyRequest->ValueSize = sizeof(LONG);
                debug(DPROP,"Wave:geometry: get -> wide(20)\n");
                ntStatus = STATUS_SUCCESS;
            } else
            {
            	debug(DWDM,"!!! Wave:geometry: get:: buffer too small\n");
                ntStatus = STATUS_BUFFER_TOO_SMALL;
            }
        } 
        else 
         if(PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
        {
            if(PropertyRequest->ValueSize >= (sizeof(KSPROPERTY_DESCRIPTION)))
            {
                // if return buffer can hold a KSPROPERTY_DESCRIPTION, return it
                PKSPROPERTY_DESCRIPTION PropDesc = PKSPROPERTY_DESCRIPTION(PropertyRequest->Value);

                PropDesc->AccessFlags       = KSPROPERTY_TYPE_BASICSUPPORT |
                                              KSPROPERTY_TYPE_GET |
                                              KSPROPERTY_TYPE_SET;
                PropDesc->DescriptionSize   = sizeof(KSPROPERTY_DESCRIPTION);
                PropDesc->PropTypeSet.Set   = KSPROPTYPESETID_General;
                PropDesc->PropTypeSet.Id    = VT_I4;
                PropDesc->PropTypeSet.Flags = 0;
                PropDesc->MembersListCount  = 0;
                PropDesc->Reserved          = 0;

                // set the return value size
                PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
                debug(DPROP,"Wave:geometry: basic support 1\n");
                ntStatus = STATUS_SUCCESS;
            } else if(PropertyRequest->ValueSize >= sizeof(ULONG))
            {
                // if return buffer can hold a ULONG, return the access flags
                PULONG AccessFlags = PULONG(PropertyRequest->Value);
        
                *AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT |
                               KSPROPERTY_TYPE_GET |
                               KSPROPERTY_TYPE_SET;
        
                // set the return value size
                PropertyRequest->ValueSize = sizeof(ULONG);
                debug(DPROP,"Wave:geometry: basic support 2\n");
                ntStatus = STATUS_SUCCESS;                    
            }
        }
        else
        if(PropertyRequest->Verb & KSPROPERTY_TYPE_SET)
        {
            if(PropertyRequest->ValueSize >= sizeof(LONG))
            {
            	if(*(PLONG)(PropertyRequest->Value)!=KSAUDIO_STEREO_SPEAKER_GEOMETRY_WIDE)
            	  debug(DWDM,"!!! trying to set Wave:geometry to %d\n",*(PLONG)(PropertyRequest->Value));
            	else
            	  debug(DPROP,"Wave:geometry:: set (std; wide=20)\n");

                ntStatus = STATUS_SUCCESS;
            } else
            {
            	debug(DWDM,"!!! Wave:geometry: set:: buffer too small\n");
                ntStatus = STATUS_BUFFER_TOO_SMALL;
            }
        } else debug(DWDM,"!!! Unknown verb\n");

    return ntStatus;
}

#pragma code_seg("PAGE")
NTSTATUS CMiniportWave::PropertySamplingRate(IN PPCPROPERTY_REQUEST PropertyRequest)
{
 PAGED_CODE();
 ASSERT(PropertyRequest);
 return CMiniportWaveOutStream::PropertySamplingRate(PropertyRequest);
}

#pragma code_seg("PAGE")
NTSTATUS CMiniportWave::PropertyPrivate(IN PPCPROPERTY_REQUEST PropertyRequest)
{
 PAGED_CODE();
 CMiniportWave *that=(CMiniportWave *)(PMINIPORTWAVECYCLIC)PropertyRequest->MajorTarget;
 if(that)
 {
  if(that->magic==WAVE_MAGIC)
   return process_property(((CAdapterCommon *)that->AdapterCommon),that->hw,PropertyRequest,WAVE_MAGIC);
 }
 return STATUS_INVALID_PARAMETER;
}

#pragma code_seg()
STDMETHODIMP_(void) CMiniportWave::PowerChangeNotify(IN      POWER_STATE     NewState)
{
    if(wave_number==0)
    {
     debug(DWDM,"[CMiniportWave::PowerChangeNotify] - process\n");

     // is this actually a state change
     if( NewState.DeviceState != PowerState )
     {
        // switch on new state
        switch( NewState.DeviceState )
        {
            case PowerDeviceD0:
                PowerState = NewState.DeviceState;
                ((CAdapterCommon *)AdapterCommon)->InterruptSync->Connect();
                kx_set_power_state(hw,KX_POWER_NORMAL);
                break;

            case PowerDeviceD1:
            case PowerDeviceD2:
                PowerState = NewState.DeviceState;
                kx_set_power_state(hw,KX_POWER_SLEEP);
                ((CAdapterCommon *)AdapterCommon)->InterruptSync->Disconnect();
                break;
                
            case PowerDeviceD3:
                PowerState = NewState.DeviceState;
                kx_set_power_state(hw,KX_POWER_SUSPEND);
                ((CAdapterCommon *)AdapterCommon)->InterruptSync->Disconnect();
                break;

            default:
                debug(DWDM,"!!! Unknown Device Power State\n");
                break;
        }
     }
    }
    else // wave_number!=0
    {
     debug(DWDM,"[CMiniportWave(%d)::PowerChangeNotify] - do nothing\n",wave_number);
     PowerState = NewState.DeviceState;
    }
}

// --------------------------------------------------------------------------------
#pragma code_seg()
void CMiniportWaveStream::my_isr(void)
{
 debug(DWDM,"!!! pure virtual call WaveStream::my_isr();\n");
}

#pragma code_seg()
NTSTATUS CMiniportWaveStream::Init(
    		  IN CMiniportWave *,
		  IN POOL_TYPE ,
    		  IN ULONG ,
    		  IN PKSDATAFORMAT ,
    		  OUT PDMACHANNEL *,
    		  OUT PSERVICEGROUP *)
{
 debug(DWDM,"!!! pure virtual call WaveStream::Init();\n");
 return STATUS_INVALID_PARAMETER;
}

#pragma code_seg("PAGE")
STDMETHODIMP CMiniportWaveStream::NonDelegatingQueryInterface(IN      REFIID  Interface,
							    OUT     PVOID * Object)
{
 PAGED_CODE();
 debug(DWDM,"!!! pure virtual call WaveStream::NonDelegating\n");
 return STATUS_INVALID_PARAMETER;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(void) CMiniportWave::PinCount
(
    IN      ULONG   PinId,
    IN  OUT PULONG  FilterNecessary,
    IN  OUT PULONG  FilterCurrent,
    IN  OUT PULONG  FilterPossible,
    IN  OUT PULONG  GlobalCurrent,
    IN  OUT PULONG  GlobalPossible
)
{
    PAGED_CODE();
    //debug(DWDM,"!!! PinCount call\n");
//
// Something like the following: FIXME
//
//    if (PinId == 0)
//    {
//        *FilterPossible += 1;
//    }
//  FilterNecessary - number of pins required on this pin factory
//  FilterCurrent   - number of pins opened on this pin factory
//  FilterPossible  - number of pins possible on this pin factory
//  GlobalCurrent   - total number of pins opened, across all pin instances on this filter
//  GlobalPossible  - total number of pins possible, across all pin factories on this filter
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CMiniportWaveStream::SetContentId(IN ULONG ContentId,IN PCDRMRIGHTS DrmRights)
{
 PAGED_CODE();
 return STATUS_SUCCESS;
}


