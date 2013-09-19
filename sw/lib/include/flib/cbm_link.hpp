#ifndef CBM_LINK_HPP
#define CBM_LINK_HPP

#include "rorc_registers.h"
#include "rorcfs_bar.hh"
#include "rorcfs_device.hh"
#include "rorcfs_buffer.hh"
#include "rorcfs_dma_channel.hh"
#include <cassert>
#include <string.h>

// has to be 256 Bit, this is hard coded in hw
struct __attribute__ ((__packed__)) MicrosliceDescriptor {
  uint8_t   hdr_id;  // "Header format identifier" DD
  uint8_t   hdr_ver; // "Header format version"    01
  uint16_t  eq_id;   // "Equipment identifier"
  uint16_t  flags;   // "Status and error flags"
  uint8_t   sys_id;  // "Subsystem identifier"
  uint8_t   sys_ver; // "Subsystem format version"
  uint64_t  idx;     // "Microslice index"
  uint32_t  crc;     // "CRC32 checksum"
  uint32_t  size;    // "Size bytes"
  uint64_t  offset;  // "Ofsset in event buffer"
};

struct __attribute__ ((__packed__)) hdr_config {
  uint16_t  eq_id;   // "Equipment identifier"
  uint8_t   sys_id;  // "Subsystem identifier"
  uint8_t   sys_ver; // "Subsystem format version"
};

struct mc_desc {
    uint64_t nr;
    volatile uint64_t* addr;
    uint32_t size; // bytes
    volatile uint64_t* rbaddr;
};

struct ctrl_msg {
  uint32_t words; // num 16 bit data words
  uint16_t data[32];
};
  
class cbm_link {
  
  rorcfs_buffer* _ebuf;
  rorcfs_buffer* _dbuf;
  rorcfs_dma_channel* _ch;
  rorcfs_device* _dev;

  uint8_t _channel; 
  uint64_t _index;
  uint64_t _last_index;
  uint64_t _last_acked;
  uint64_t _mc_nr;
  uint64_t _wrap;
  
  volatile uint64_t* _eb;
  volatile struct MicrosliceDescriptor* _db;
  
  uint64_t _dbsize;
  uint64_t _dbentries;
  
public:
  
  //    struct mc_desc mc;
  
    cbm_link(uint8_t channel, rorcfs_device* dev, rorcfs_bar* bar) 
      :  _ebuf(NULL), _dbuf(NULL), _index(0), _last_index(0), _last_acked(0), _mc_nr(0), _wrap(0) { 
      
      _channel = channel;
      _dev = dev;
      // create DMA channel and bind to BAR1, no HW initialization is done here
      _ch = new rorcfs_dma_channel();
      _ch->init(bar, (_channel+1)*RORC_CHANNEL_OFFSET);
    }

    ~cbm_link() {
      if(_ch) {
        // disable DMA Engine
        _ch->setEnableEB(0);
        // wait for pending transfers to complete (dma_busy->0)
        //while( _ch->getDMABusy() ) TODO
         usleep(100);
        // disable RBDM
        _ch->setEnableRB(0);
        // disable DMA PKT
        //TODO: if resetting DFIFO by setting 0x2 restart is impossible
        _ch->setDMAConfig(0X00000000);
      }
      if(_ebuf){
        if(_ebuf->deallocate() != 0) {
          std::cout << "ERROR: ebuf->deallocate" << std::endl;
          throw 1;
        }
        delete _ebuf;
        _ebuf = NULL;
      }
      if(_dbuf){
        if(_dbuf->deallocate() != 0) {
          std::cout << "ERROR: rbuf->deallocate" << std::endl;
          throw 1;
        }
        delete _dbuf;
        _dbuf = NULL;
      }
      if(_ch) {
        delete _ch;
        _ch = NULL;
      }
    }  

  int init_dma(size_t log_ebufsize, size_t log_dbufsize) {

    // Buffer sizes in bytes
    unsigned long ebufsize = (((unsigned long)1) << log_ebufsize);
    unsigned long rbufsize = (((unsigned long)1) << log_dbufsize);

        // create new DMA event buffer
        _ebuf = new rorcfs_buffer();			
        if (_ebuf->allocate(_dev, ebufsize, 2*_channel, 
                            1, RORCFS_DMA_FROM_DEVICE)!=0) {
            if (errno == EEXIST) {
                printf("INFO: Buffer ebuf %d already exists, trying to connect ...\n", 2*_channel);
                if ( _ebuf->connect(_dev, 2*_channel) != 0 ) {
                    perror("ERROR: ebuf->connect");
                    return -1;
                }
            } else {
                perror("ERROR: ebuf->allocate");
                return -1;
            }
        }
        printf("INFO: pEBUF=%p, PhysicalSize=%ld MBytes, MappingSize=%ld MBytes,\n"
               "    EndAddr=%p, nSGEntries=%ld\n", 
               (void *)_ebuf->getMem(), _ebuf->getPhysicalSize() >> 20,
               _ebuf->getMappingSize() >> 20, 
               (uint8_t *)_ebuf->getMem() + _ebuf->getPhysicalSize(), 
               _ebuf->getnSGEntries());

        // create new DMA report buffer
        _dbuf = new rorcfs_buffer();
        if (_dbuf->allocate(_dev, rbufsize, 2*_channel+1, 
                            1, RORCFS_DMA_FROM_DEVICE)!=0) {
            if (errno == EEXIST) {
                printf("INFO: Buffer rbuf %d already exists, trying to connect ...\n",
                       2*_channel+1);
                if (_dbuf->connect(_dev, 2*_channel+1) != 0) {
                    perror("ERROR: rbuf->connect");
                    return -1;
                }
            } else {
                perror("ERROR: rbuf->allocate");
                return -1;
            }
        }
        printf("INFO: pRBUF=%p, PhysicalSize=%ld MBytes, MappingSize=%ld MBytes,\n"
               "    EndAddr=%p, nSGEntries=%ld, MaxRBEntries=%ld\n", 
               (void *)_dbuf->getMem(), _dbuf->getPhysicalSize() >> 20,
               _dbuf->getMappingSize() >> 20, 
               (uint8_t *)_dbuf->getMem() + _dbuf->getPhysicalSize(), 
               _dbuf->getnSGEntries(), _dbuf->getMaxRBEntries() );

        // prepare EventBufferDescriptorManager
        // and ReportBufferDescriptorManage
        // with scatter-gather list
        if( _ch->prepareEB(_ebuf) < 0 ) {
            perror("prepareEB()");
            return -1;
        }
        if( _ch->prepareRB(_dbuf) < 0 ) {
            perror("prepareRB()");
            return -1;
        }

        if( _ch->configureChannel(_ebuf, _dbuf, 128) < 0) {
            perror("configureChannel()");
            return -1;
        }

        // clear eb for debugging
        memset(_ebuf->getMem(), 0, _ebuf->getMappingSize());
        // clear rb for polling
        memset(_dbuf->getMem(), 0, _dbuf->getMappingSize());

        _eb = (uint64_t *)_ebuf->getMem();
        _db = (struct MicrosliceDescriptor *)_dbuf->getMem();
        
        // TODO check if sizes can be deduced from init velues
        _dbsize = _dbuf->getPhysicalSize();
        _dbentries = _dbuf->getMaxRBEntries();

        // Enable desciptor buffers and dma engine
        _ch->setEnableEB(1);
        _ch->setEnableRB(1);
        _ch->setDMAConfig( _ch->getDMAConfig() | 0x01 );
    
        return 0;
    };

    int enable_TODO() {
      // hold data source here
        return 0; 
    }

    int disable_TODO() {
        return 0;
    }
  
    std::pair<mc_desc, bool> get_mc() {
        struct mc_desc mc;
        if(_db[_index].idx > _mc_nr) { // mc_nr counts from 1 in HW
          _mc_nr = _db[_index].idx;
          mc.nr = _mc_nr;
          mc.addr = _eb + _db[_index].offset/sizeof(uint64_t);
          mc.size = _db[_index].size;
          mc.rbaddr = (uint64_t *)&_db[_index];

          // calculate next rb index
          _last_index = _index;
          if( _index < _dbentries-1 ) 
            _index++;
          else {
            _wrap++;
            _index = 0;
          }
          return std::make_pair(mc, true);
        }
        else
          return std::make_pair(mc, false);
    }
  
  int ack_mc() {
        
    // TODO: EB pointers are set to begin of acknoledged entry, pointers are one entry delayed
    // to calculate end wrapping logic is required
    uint64_t eb_offset = _db[_last_index].offset;
    // each rbenty is 32 bytes, this is hard coded in HW
    uint64_t rb_offset = _last_index*sizeof(struct MicrosliceDescriptor) % _dbsize;

    //_ch->setEBOffset(eb_offset);
    //_ch->setRBOffset(rb_offset);

    _ch->setOffsets(eb_offset, rb_offset);

#ifdef DEBUG  
    printf("index %d EB offset set: %ld, get: %ld\n",
           _last_index, eb_offset, _ch->getEBOffset());
    printf("index %d RB offset set: %ld, get: %ld, wrap %d\n",
           _last_index, rb_offset, _ch->getRBOffset(), _wrap);
#endif

    return 0;
    }
  
    // TODO: Add funtions to set channel properties like data source, link reset, activate busys 

  // REG: mc_gen_cfg
  // bit 0 set_start_index
  // bit 1 rst_pending_mc
  // bit 2 packer enable

  void set_start_idx(uint64_t index) {
    // set reset value
    _ch->setGTX(RORC_REG_GTX_MC_GEN_CFG_IDX_L, (uint32_t)(index & 0xffffffff));
    _ch->setGTX(RORC_REG_GTX_MC_GEN_CFG_IDX_H, (uint32_t)(index >> 32));
    // reste mc counter 
    // TODO implenet edge detection and 'pulse only' in HW
    uint32_t mc_gen_cfg= _ch->getGTX(RORC_REG_GTX_MC_GEN_CFG);
    _ch->setGTX(RORC_REG_GTX_MC_GEN_CFG, (mc_gen_cfg | 1));
    _ch->setGTX(RORC_REG_GTX_MC_GEN_CFG, (mc_gen_cfg & ~(1)));
  }

  void rst_pending_mc() {
    // TODO implenet edge detection and 'pulse only' in HW
    uint32_t mc_gen_cfg= _ch->getGTX(RORC_REG_GTX_MC_GEN_CFG);
    _ch->setGTX(RORC_REG_GTX_MC_GEN_CFG, (mc_gen_cfg | (1<<1)));
    _ch->setGTX(RORC_REG_GTX_MC_GEN_CFG, (mc_gen_cfg & ~(1<<1)));
  }

  void enable_cbmnet_packer(bool enable) {
    _ch->set_bitGTX(RORC_REG_GTX_MC_GEN_CFG, 2, enable);
  }

  uint64_t get_pending_mc() {
    uint64_t pend_mc = _ch->getGTX(RORC_REG_GTX_PENDING_MC_L);
    pend_mc = pend_mc | ((uint64_t)(_ch->getGTX(RORC_REG_GTX_PENDING_MC_H))<<32);
    return pend_mc;
  }

  uint64_t get_mc_index() {
    uint64_t mc_index = _ch->getGTX(RORC_REG_GTX_MC_INDEX_L);
    mc_index = mc_index | ((uint64_t)(_ch->getGTX(RORC_REG_GTX_MC_INDEX_H))<<32);
    return mc_index;
  }

  // REG: datapath_cfg
  // bit 0-1 data_rx_sel (10: link, 11: pgen, 01: emu, 00: disable)
  enum data_rx_sel {disable, link, pgen, emu};

  void set_data_rx_sel(data_rx_sel rx_sel) {
    uint32_t dp_cfg = _ch->getGTX(RORC_REG_GTX_DATAPATH_CFG);
    switch (rx_sel) {
    case disable : _ch->setGTX(RORC_REG_GTX_DATAPATH_CFG, (dp_cfg & ~3)); break;
    case link :    _ch->setGTX(RORC_REG_GTX_DATAPATH_CFG, ((dp_cfg | (1<<1)) & ~1) ); break;
    case pgen :    _ch->setGTX(RORC_REG_GTX_DATAPATH_CFG, (dp_cfg | 3) ); break;
    case emu :     _ch->setGTX(RORC_REG_GTX_DATAPATH_CFG, ((dp_cfg | 1)) & ~(1<<1)); break;
    }
  }

  void set_hdr_config(struct hdr_config* config) {
    _ch->set_memGTX(RORC_REG_GTX_MC_GEN_CFG_HDR, (const void*)config, sizeof(hdr_config));
  }

  // Control Interface ////////////////////////////

  int send_msg(const struct ctrl_msg* msg) {
    // TODO: could also implement blocking call 
    //       and check if sending is done at the end

    assert (msg->words >= 4 && msg->words <= 32);
    
    // check if send FSM is ready (bit 31 in r_ctrl_tx = 0)
    if ( (_ch->getGTX(RORC_REG_GTX_CTRL_TX) & (1<<31)) != 0 ) {
      return -1;
    }
    
    // copy msg to board memory
    size_t bytes = msg->words*2 + (msg->words*2)%4;
    _ch->set_memGTX(RORC_MEM_BASE_CTRL_TX, (const void*)msg->data, bytes);
    
    // start send FSM
    uint32_t ctrl_tx = 0;
    ctrl_tx = 1<<31 | (msg->words-1);
    _ch->setGTX(RORC_REG_GTX_CTRL_TX, ctrl_tx);
    printf("set ctrl_tx: %08x\n", ctrl_tx);
    
    return 0;
}

  int rcv_msg(struct ctrl_msg* msg) {
    
    int ret = 0;
    uint32_t ctrl_rx = _ch->getGTX(RORC_REG_GTX_CTRL_RX);
    msg->words = (ctrl_rx & 0x1F)+1;
    printf("r_ctrl_tx %08x\n", ctrl_rx);
    
    // check if received words are in boundary
    if (msg->words < 4 || msg->words > 32) {
      msg->words = 32;
      ret = -2;
    }
    // check if a msg is available
    if ((ctrl_rx & (1<<31)) == 0) {
      return -1;
    }
    
    // read msg from board memory
    size_t bytes = msg->words*2 + (msg->words*2)%4;
    _ch->get_memGTX(RORC_MEM_BASE_CTRL_RX, (void*)msg->data, bytes);
    
    // acknowledge msg
    _ch->setGTX(RORC_REG_GTX_CTRL_RX, 0);
    printf("ctrl_rx cleard\n");
    
    return ret;
  } 

  // RORC_REG_DLM_CFG 0 set to send
  // RORC_REG_GTX_DLM 3..0 tx type, 4 enable,
  //                  8..5 rx type, 31 set to clear rx reg
  void set_dlm_cfg(uint8_t type, bool enable) {
    uint32_t reg = 0;
    if (enable)
      reg = (1<<4) | (type & 0xF);
    else
      reg = (type & 0xF);
    _ch->setGTX(RORC_REG_GTX_DLM, reg);
  }

  uint8_t get_dlm() {
    uint32_t reg = _ch->getGTX(RORC_REG_GTX_DLM);
    uint8_t type = (reg>>5) & 0xF;
    return type;
  }

  void clr_dlm() {
    _ch->setGTX(RORC_REG_GTX_DLM, 1<<31);
  }

    rorcfs_buffer* ebuf() const {
        return _ebuf;
    }

    rorcfs_buffer* rbuf() const {
        return _dbuf;
    }

    rorcfs_dma_channel* get_ch() const {
        return _ch;
    }
};

#endif // CBM_LINK_HPP
