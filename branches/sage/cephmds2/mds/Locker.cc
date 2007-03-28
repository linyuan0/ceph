// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#include "MDS.h"
#include "MDCache.h"
#include "Locker.h"
#include "Server.h"
#include "CInode.h"
#include "CDir.h"
#include "CDentry.h"
#include "Migrator.h"

#include "MDBalancer.h"
#include "MDLog.h"
#include "MDSMap.h"

#include "include/filepath.h"

#include "events/EString.h"
#include "events/EUpdate.h"
#include "events/EUnlink.h"

#include "msg/Messenger.h"

#include "messages/MGenericMessage.h"
#include "messages/MDiscover.h"
#include "messages/MDiscoverReply.h"

#include "messages/MDirUpdate.h"

#include "messages/MInodeFileCaps.h"

#include "messages/MInodeLink.h"
#include "messages/MInodeLinkAck.h"
#include "messages/MInodeUnlink.h"
#include "messages/MInodeUnlinkAck.h"

#include "messages/MLock.h"
#include "messages/MDentryUnlink.h"

#include "messages/MClientRequest.h"
#include "messages/MClientFileCaps.h"

#include <errno.h>
#include <assert.h>

#include "config.h"
#undef dout
#define  dout(l)    if (l<=g_conf.debug || l <= g_conf.debug_mds) cout << g_clock.now() << " mds" << mds->get_nodeid() << ".locker "



void Locker::dispatch(Message *m)
{
  switch (m->get_type()) {

    // locking
  case MSG_MDS_LOCK:
    handle_lock((MLock*)m);
    break;

    // cache fun
  case MSG_MDS_INODEFILECAPS:
    handle_inode_file_caps((MInodeFileCaps*)m);
    break;

  case MSG_CLIENT_FILECAPS:
    handle_client_file_caps((MClientFileCaps*)m);
    break;

    

  default:
    assert(0);
  }
}


void Locker::send_lock_message(CInode *in, int msg, int type)
{
  for (map<int,int>::iterator it = in->replicas_begin(); 
       it != in->replicas_end(); 
       it++) {
    MLock *m = new MLock(msg, mds->get_nodeid());
    m->set_ino(in->ino(), type);
    mds->send_message_mds(m, it->first, MDS_PORT_LOCKER);
  }
}


void Locker::send_lock_message(CInode *in, int msg, int type, bufferlist& data)
{
  for (map<int,int>::iterator it = in->replicas_begin(); 
       it != in->replicas_end(); 
       it++) {
    MLock *m = new MLock(msg, mds->get_nodeid());
    m->set_ino(in->ino(), type);
    m->set_data(data);
    mds->send_message_mds(m, it->first, MDS_PORT_LOCKER);
  }
}

void Locker::send_lock_message(CDentry *dn, int msg)
{
  for (map<int,int>::iterator it = dn->replicas_begin();
       it != dn->replicas_end();
       it++) {
    MLock *m = new MLock(msg, mds->get_nodeid());
    m->set_dn(dn->dir->dirfrag(), dn->name);
    mds->send_message_mds(m, it->first, MDS_PORT_LOCKER);
  }
}











bool Locker::acquire_locks(MDRequest *mdr,
			   set<CDentry*> &dentry_rdlocks,
			   set<CDentry*> &dentry_xlocks,
			   set<CInode*> &inode_hard_rdlocks,
			   set<CInode*> &inode_hard_xlocks)
{
  dout(10) << "acquire_locks " << *mdr << endl;

  // (local) AUTH PINS

  // can i auth_pin everything?
  for (set<CDentry*>::iterator p = dentry_xlocks.begin();
       p != dentry_xlocks.end();
       ++p) {
    CDir *dir = (*p)->dir;
    if (!dir->is_auth()) continue;
    if (!mdr->is_auth_pinned(dir) &&
	!dir->can_auth_pin()) {
      // wait
      dir->add_waiter(CDir::WAIT_AUTHPINNABLE, new C_MDS_RetryRequest(mdcache, mdr));
      mdcache->request_drop_locks(mdr);
      mdr->drop_auth_pins();
      return false;
    }
  }
  for (set<CInode*>::iterator p = inode_hard_xlocks.begin();
       p != inode_hard_xlocks.end();
       ++p) {
    CInode *in = *p;
    if (!in->is_auth()) continue;
    if (!mdr->is_auth_pinned(in) &&
	!in->can_auth_pin()) {
      in->add_waiter(CInode::WAIT_AUTHPINNABLE, new C_MDS_RetryRequest(mdcache, mdr));
      mdcache->request_drop_locks(mdr);
      mdr->drop_auth_pins();
      return false;
    }
  }

  // ok, grab the auth pins
  for (set<CDentry*>::iterator p = dentry_xlocks.begin();
       p != dentry_xlocks.end();
       ++p) {
    CDir *dir = (*p)->dir;
    if (!dir->is_auth()) continue;
    mdr->auth_pin(dir);
  }
  for (set<CInode*>::iterator p = inode_hard_xlocks.begin();
       p != inode_hard_xlocks.end();
       ++p) {
    CInode *in = *p;
    if (!in->is_auth()) continue;
    mdr->auth_pin(in);
  }


  // DENTRY LOCKS
  {
    // sort all the dentries we will lock
    set<CDentry*, CDentry::ptr_lt> sorted;
    for (set<CDentry*>::iterator p = dentry_xlocks.begin();
	 p != dentry_xlocks.end();
	 ++p) {
      dout(10) << "will xlock " << **p << endl;
      sorted.insert(*p);
    }
    for (set<CDentry*>::iterator p = dentry_rdlocks.begin();
	 p != dentry_rdlocks.end();
	 ++p) {
      dout(10) << "will rdlock " << **p << endl;
      sorted.insert(*p);
    }
    
    // acquire dentry locks.  make sure they match currently acquired locks.
    set<CDentry*, CDentry::ptr_lt>::iterator existing = mdr->dentry_locks.begin();
    for (set<CDentry*, CDentry::ptr_lt>::iterator p = sorted.begin();
	 p != sorted.end();
	 ++p) {

      // already locked?
      if (existing != mdr->dentry_locks.end() && *existing == *p) {
	// right kind?
	CDentry *had = *existing;
	if (dentry_xlocks.count(*p) == had->is_xlockedbyme(mdr)) {
	  dout(10) << "acquire_locks already locked " << *had << endl;
	  existing++;
	  continue;
	}
      }

      // hose any stray locks
      while (existing != mdr->dentry_locks.end()) {
	CDentry *had = *existing;
	existing++;
	dout(10) << "acquire_locks had " << *had << " locked before " << **p 
		 << ", unlocking" << endl;
	if (had->is_xlockedbyme(mdr))
	  dentry_xlock_finish(had, mdr);
	else
	  dentry_rdlock_finish(had, mdr);
      }
      
      // lock
      if (dentry_xlocks.count(*p)) {
	if (!dentry_xlock_start(*p, mdr))
	  return false;
	dout(10) << "acquire_locks got xlock on " << **p << endl;
      } else {
	if (!dentry_rdlock_start(*p, mdr))
	  return false;
	dout(10) << "acquire_locks got rdlock on " << **p << endl;
      }
    }
    
    // any extra unneeded locks?
    while (existing != mdr->dentry_locks.end()) {
      dout(10) << "acquire_locks had " << *existing << " locked, unlocking" << endl;
      if ((*existing)->is_xlockedbyme(mdr))
	dentry_xlock_finish(*existing, mdr);
      else
	dentry_rdlock_finish(*existing, mdr);
    }
  }

  // INODES
  {
    // sort all the dentries we will lock
    set<CInode*, CInode::ptr_lt> sorted;
    for (set<CInode*>::iterator p = inode_hard_xlocks.begin();
	 p != inode_hard_xlocks.end();
	 ++p) 
      sorted.insert(*p);
    for (set<CInode*>::iterator p = inode_hard_rdlocks.begin();
	 p != inode_hard_rdlocks.end();
	 ++p) 
      sorted.insert(*p);
    
    // acquire inode locks.  make sure they match currently acquired locks.
    set<CInode*, CInode::ptr_lt>::iterator existing = mdr->inode_hard_locks.begin();
    for (set<CInode*, CInode::ptr_lt>::iterator p = sorted.begin();
	 p != sorted.end();
	 ++p) {
      // already locked?
      if (existing != mdr->inode_hard_locks.end() && *existing == *p) {
	// right kind?
	CInode *had = *existing;
	if (inode_hard_xlocks.count(*p) == (had->hardlock.get_wrlocked_by() == mdr)) {
	  dout(10) << "acquire_locks already locked " << *had << endl;
	  existing++;
	  continue;
	}
      }

      // hose any stray locks
      while (existing != mdr->inode_hard_locks.end()) {
	CInode *had = *existing;
	existing++;
	dout(10) << "acquire_locks had " << *had << " locked before " << **p 
		 << ", unlocking" << endl;
	if (had->hardlock.get_wrlocked_by() == mdr)
	  inode_hard_xlock_finish(had, mdr);
	else
	  inode_hard_rdlock_finish(had, mdr);
      }
      
      // lock
      if (inode_hard_xlocks.count(*p)) {
	if (!inode_hard_xlock_start(*p, mdr))
	  return false;
	dout(10) << "acquire_locks got xlock on " << **p << endl;
      } else {
	if (!inode_hard_rdlock_start(*p, mdr))
	  return false;
	dout(10) << "acquire_locks got rdlock on " << **p << endl;
      }
    }
    
    // any extra unneeded locks?
    while (existing != mdr->inode_hard_locks.end()) {
      dout(10) << "acquire_locks had " << **existing << " locked, unlocking" << endl;
      if ((*existing)->hardlock.get_wrlocked_by() == mdr)
	inode_hard_xlock_finish(*existing, mdr);
      else
	inode_hard_rdlock_finish(*existing, mdr);
    }
  }

  return true;
}










// file i/o -----------------------------------------

__uint64_t Locker::issue_file_data_version(CInode *in)
{
  dout(7) << "issue_file_data_version on " << *in << endl;
  return in->inode.file_data_version;
}


Capability* Locker::issue_new_caps(CInode *in,
                                    int mode,
                                    MClientRequest *req)
{
  dout(7) << "issue_new_caps for mode " << mode << " on " << *in << endl;
  
  // my needs
  int my_client = req->get_client();
  int my_want = 0;
  if (mode & FILE_MODE_R) my_want |= CAP_FILE_RDCACHE  | CAP_FILE_RD;
  if (mode & FILE_MODE_W) my_want |= CAP_FILE_WRBUFFER | CAP_FILE_WR;

  // register a capability
  Capability *cap = in->get_client_cap(my_client);
  if (!cap) {
    // new cap
    Capability c(my_want);
    in->add_client_cap(my_client, c);
    cap = in->get_client_cap(my_client);
    
    // note client addr
    mds->clientmap.add_open(my_client, req->get_client_inst());
    
  } else {
    // make sure it has sufficient caps
    if (cap->wanted() & ~my_want) {
      // augment wanted caps for this client
      cap->set_wanted( cap->wanted() | my_want );
    }
  }

  // suppress file cap messages for this guy for a few moments (we'll bundle with the open() reply)
  cap->set_suppress(true);
  int before = cap->pending();

  if (in->is_auth()) {
    // [auth] twiddle mode?
    inode_file_eval(in);
  } else {
    // [replica] tell auth about any new caps wanted
    request_inode_file_caps(in);
  }
    
  // issue caps (pot. incl new one)
  issue_caps(in);  // note: _eval above may have done this already...

  // re-issue whatever we can
  cap->issue(cap->pending());
  
  // ok, stop suppressing.
  cap->set_suppress(false);

  int now = cap->pending();
  if (before != now &&
      (before & CAP_FILE_WR) == 0 &&
      (now & CAP_FILE_WR)) {
    // FIXME FIXME FIXME
  }
  
  // twiddle file_data_version?
  if ((before & CAP_FILE_WRBUFFER) == 0 &&
      (now & CAP_FILE_WRBUFFER)) {
    in->inode.file_data_version++;
    dout(7) << " incrementing file_data_version, now " << in->inode.file_data_version << " for " << *in << endl;
  }

  return cap;
}



bool Locker::issue_caps(CInode *in)
{
  // allowed caps are determined by the lock mode.
  int allowed = in->filelock.caps_allowed(in->is_auth());
  dout(7) << "issue_caps filelock allows=" << cap_string(allowed) 
          << " on " << *in << endl;

  // count conflicts with
  int nissued = 0;        

  // client caps
  for (map<int, Capability>::iterator it = in->client_caps.begin();
       it != in->client_caps.end();
       it++) {
    if (it->second.issued() != (it->second.wanted() & allowed)) {
      // issue
      nissued++;

      int before = it->second.pending();
      long seq = it->second.issue(it->second.wanted() & allowed);
      int after = it->second.pending();

      // twiddle file_data_version?
      if (!(before & CAP_FILE_WRBUFFER) &&
          (after & CAP_FILE_WRBUFFER)) {
        dout(7) << "   incrementing file_data_version for " << *in << endl;
        in->inode.file_data_version++;
      }

      if (seq > 0 && 
          !it->second.is_suppress()) {
        dout(7) << "   sending MClientFileCaps to client" << it->first << " seq " << it->second.get_last_seq() << " new pending " << cap_string(it->second.pending()) << " was " << cap_string(before) << endl;
        mds->messenger->send_message(new MClientFileCaps(in->inode,
                                                         it->second.get_last_seq(),
                                                         it->second.pending(),
                                                         it->second.wanted()),
                                     mds->clientmap.get_inst(it->first), 
				     0, MDS_PORT_LOCKER);
      }
    }
  }

  return (nissued == 0);  // true if no re-issued, no callbacks
}



void Locker::request_inode_file_caps(CInode *in)
{
  int wanted = in->get_caps_wanted();
  if (wanted != in->replica_caps_wanted) {

    if (wanted == 0) {
      if (in->replica_caps_wanted_keep_until > g_clock.recent_now()) {
        // ok, release them finally!
        in->replica_caps_wanted_keep_until.sec_ref() = 0;
        dout(7) << "request_inode_file_caps " << cap_string(wanted)
                 << " was " << cap_string(in->replica_caps_wanted) 
                 << " no keeping anymore " 
                 << " on " << *in 
                 << endl;
      }
      else if (in->replica_caps_wanted_keep_until.sec() == 0) {
        in->replica_caps_wanted_keep_until = g_clock.recent_now();
        in->replica_caps_wanted_keep_until.sec_ref() += 2;
        
        dout(7) << "request_inode_file_caps " << cap_string(wanted)
                 << " was " << cap_string(in->replica_caps_wanted) 
                 << " keeping until " << in->replica_caps_wanted_keep_until
                 << " on " << *in 
                 << endl;
        return;
      } else {
        // wait longer
        return;
      }
    } else {
      in->replica_caps_wanted_keep_until.sec_ref() = 0;
    }
    assert(!in->is_auth());

    int auth = in->authority().first;
    dout(7) << "request_inode_file_caps " << cap_string(wanted)
            << " was " << cap_string(in->replica_caps_wanted) 
            << " on " << *in << " to mds" << auth << endl;
    assert(!in->is_auth());

    in->replica_caps_wanted = wanted;
    mds->send_message_mds(new MInodeFileCaps(in->ino(), mds->get_nodeid(),
					     in->replica_caps_wanted),
			  auth, MDS_PORT_LOCKER);
  } else {
    in->replica_caps_wanted_keep_until.sec_ref() = 0;
  }
}

void Locker::handle_inode_file_caps(MInodeFileCaps *m)
{
  CInode *in = mdcache->get_inode(m->get_ino());
  assert(in);
  assert(in->is_auth());// || in->is_proxy());
  
  dout(7) << "handle_inode_file_caps replica mds" << m->get_from() << " wants caps " << cap_string(m->get_caps()) << " on " << *in << endl;

  /*if (in->is_proxy()) {
    dout(7) << "proxy, fw" << endl;
    mds->send_message_mds(m, in->authority().first, MDS_PORT_LOCKER);
    return;
  }
  */

  if (m->get_caps())
    in->mds_caps_wanted[m->get_from()] = m->get_caps();
  else
    in->mds_caps_wanted.erase(m->get_from());

  inode_file_eval(in);
  delete m;
}


/*
 * note: we only get these from the client if
 * - we are calling back previously issued caps (fewer than the client previously had)
 * - or if the client releases (any of) its caps on its own
 */
void Locker::handle_client_file_caps(MClientFileCaps *m)
{
  int client = m->get_source().num();
  CInode *in = mdcache->get_inode(m->get_ino());
  Capability *cap = 0;
  if (in) 
    cap = in->get_client_cap(client);

  if (!in || !cap) {
    if (!in) {
      dout(7) << "handle_client_file_caps on unknown ino " << m->get_ino() << ", dropping" << endl;
    } else {
      dout(7) << "handle_client_file_caps no cap for client" << client << " on " << *in << endl;
    }
    delete m;
    return;
  } 
  
  assert(cap);

  // filter wanted based on what we could ever give out (given auth/replica status)
  int wanted = m->get_wanted() & in->filelock.caps_allowed_ever(in->is_auth());
  
  dout(7) << "handle_client_file_caps seq " << m->get_seq() 
          << " confirms caps " << cap_string(m->get_caps()) 
          << " wants " << cap_string(wanted)
          << " from client" << client
          << " on " << *in 
          << endl;  
  
  // update wanted
  if (cap->wanted() != wanted)
    cap->set_wanted(wanted);

  // confirm caps
  int had = cap->confirm_receipt(m->get_seq(), m->get_caps());
  int has = cap->confirmed();
  if (cap->is_null()) {
    dout(7) << " cap for client" << client << " is now null, removing from " << *in << endl;
    in->remove_client_cap(client);
    if (!in->is_auth())
      request_inode_file_caps(in);

    // dec client addr counter
    mds->clientmap.dec_open(client);

    // tell client.
    MClientFileCaps *r = new MClientFileCaps(in->inode, 
                                             0, 0, 0,
                                             MClientFileCaps::FILECAP_RELEASE);
    mds->messenger->send_message(r, m->get_source_inst(), 0, MDS_PORT_LOCKER);
  }

  // merge in atime?
  if (m->get_inode().atime > in->inode.atime) {
      dout(7) << "  taking atime " << m->get_inode().atime << " > " 
              << in->inode.atime << " for " << *in << endl;
    in->inode.atime = m->get_inode().atime;
  }
  
  if ((has|had) & CAP_FILE_WR) {
    bool dirty = false;

    // mtime
    if (m->get_inode().mtime > in->inode.mtime) {
      dout(7) << "  taking mtime " << m->get_inode().mtime << " > " 
              << in->inode.mtime << " for " << *in << endl;
      in->inode.mtime = m->get_inode().mtime;
      dirty = true;
    }
    // size
    if (m->get_inode().size > in->inode.size) {
      dout(7) << "  taking size " << m->get_inode().size << " > " 
              << in->inode.size << " for " << *in << endl;
      in->inode.size = m->get_inode().size;
      dirty = true;
    }

    if (dirty) 
      mds->mdlog->submit_entry(new EString("cap inode update dirty fixme"));
  }  

  // reevaluate, waiters
  inode_file_eval(in);
  in->finish_waiting(CInode::WAIT_CAPS, 0);

  delete m;
}










// locks ----------------------------------------------------------------

/*


INODES:

= two types of inode metadata:
   hard  - uid/gid, mode
   file  - mtime, size
 ? atime - atime  (*)       <-- we want a lazy update strategy?

= correspondingly, two types of inode locks:
   hardlock - hard metadata
   filelock - file metadata

   -> These locks are completely orthogonal! 

= metadata ops and how they affect inode metadata:
        sma=size mtime atime
   HARD FILE OP
  files:
    R   RRR stat
    RW      chmod/chown
    R    W  touch   ?ctime
    R       openr
          W read    atime
    R       openw
    Wc      openwc  ?ctime
        WW  write   size mtime
            close 

  dirs:
    R     W readdir atime 
        RRR  ( + implied stats on files)
    Rc  WW  mkdir         (ctime on new dir, size+mtime on parent dir)
    R   WW  link/unlink/rename/rmdir  (size+mtime on dir)

  

= relationship to client (writers):

  - ops in question are
    - stat ... need reasonable value for mtime (+ atime?)
      - maybe we want a "quicksync" type operation instead of full lock
    - truncate ... need to stop writers for the atomic truncate operation
      - need a full lock




= modes
  - SYNC
              Rauth  Rreplica  Wauth  Wreplica
        sync
        




ALSO:

  dirlock  - no dir changes (prior to unhashing)
  denlock  - dentry lock    (prior to unlink, rename)

     
*/


void Locker::handle_lock(MLock *m)
{
  switch (m->get_otype()) {
  case LOCK_OTYPE_IHARD:
    handle_lock_inode_hard(m);
    break;
    
  case LOCK_OTYPE_IFILE:
    handle_lock_inode_file(m);
    break;
    
  case LOCK_OTYPE_DIR:
    handle_lock_dir(m);
    break;
    
  case LOCK_OTYPE_DN:
    handle_lock_dn(m);
    break;

  default:
    dout(7) << "handle_lock got otype " << m->get_otype() << endl;
    assert(0);
    break;
  }
}
 


// ===============================
// hard inode metadata

bool Locker::inode_hard_rdlock_try(CInode *in, Context *con)
{
  dout(7) << "inode_hard_rdlock_try on " << *in << endl;  

  // can read?  grab ref.
  if (in->hardlock.can_read(in->is_auth())) 
    return true;
  
  assert(!in->is_auth());

  // wait!
  dout(7) << "inode_hard_rdlock_try waiting on " << *in << endl;
  in->add_waiter(CInode::WAIT_HARDR, con);
  return false;
}

bool Locker::inode_hard_rdlock_start(CInode *in, MDRequest *mdr)
{
  dout(7) << "inode_hard_rdlock_start  on " << *in << endl;  

  // can read?  grab ref.
  if (in->hardlock.can_read(in->is_auth())) {
    in->hardlock.get_read();
    mdr->inode_hard_rdlocks.insert(in);
    mdr->inode_hard_locks.insert(in);
    return true;
  }
  
  // can't read, and replicated.
  assert(!in->is_auth());

  // wait!
  dout(7) << "inode_hard_rdlock_start waiting on " << *in << endl;
  in->add_waiter(CInode::WAIT_HARDR, new C_MDS_RetryRequest(mdcache, mdr));
  return false;
}


void Locker::inode_hard_rdlock_finish(CInode *in, MDRequest *mdr)
{
  // drop ref
  assert(in->hardlock.can_read(in->is_auth()));
  in->hardlock.put_read();
  mdr->inode_hard_rdlocks.erase(in);
  mdr->inode_hard_locks.erase(in);

  dout(7) << "inode_hard_rdlock_finish on " << *in << endl;
  
  //if (in->hardlock.get_nread() == 0) in->finish_waiting(CInode::WAIT_HARDNORD);
}


bool Locker::inode_hard_xlock_start(CInode *in, MDRequest *mdr)
{
  dout(7) << "inode_hard_xlock_start  on " << *in << endl;

  // if not replicated, i can twiddle lock at will
  if (in->is_auth() &&
      !in->is_replicated() &&
      in->hardlock.get_state() != LOCK_LOCK) 
    in->hardlock.set_state(LOCK_LOCK);
  
  // can write?  grab ref.
  if (in->hardlock.can_write(in->is_auth())) {
    assert(in->is_auth());
    in->hardlock.get_write(mdr);
    mdr->inode_hard_xlocks.insert(in);
    mdr->inode_hard_locks.insert(in);
    return true;
  }
  
  // can't write, replicated.
  if (in->is_auth()) {
    // auth
    if (in->hardlock.can_write_soon(in->is_auth())) {
      // just wait
    } else {
      // initiate lock
      inode_hard_lock(in);
    }
    
    dout(7) << "inode_hard_xlock_start waiting on " << *in << endl;
    in->add_waiter(CInode::WAIT_HARDW, new C_MDS_RetryRequest(mdcache, mdr));

    return false;
  } else {
    // replica
    // fw to auth
    int auth = in->authority().first;
    dout(7) << "inode_hard_xlock_start " << *in << " on replica, fw to auth " << auth << endl;
    assert(auth != mds->get_nodeid());
    mdcache->request_forward(mdr, auth);
    return false;
  }
}


void Locker::inode_hard_xlock_finish(CInode *in, MDRequest *mdr)
{
  // drop ref
  //assert(in->hardlock.can_write(in->is_auth()));
  in->hardlock.put_write();
  mdr->inode_hard_xlocks.erase(in);
  mdr->inode_hard_locks.erase(in);
  dout(7) << "inode_hard_xlock_finish on " << *in << endl;

  // others waiting?
  if (in->is_hardlock_write_wanted()) {
    // wake 'em up
    in->take_waiting(CInode::WAIT_HARDW, mds->finished_queue);
  } else {
    // auto-sync if alone.
    if (in->is_auth() &&
        !in->is_replicated() &&
        in->hardlock.get_state() != LOCK_SYNC) 
      in->hardlock.set_state(LOCK_SYNC);
    
    inode_hard_eval(in);
  }
}


void Locker::inode_hard_eval(CInode *in)
{
  // finished gather?
  if (in->is_auth() &&
      !in->hardlock.is_stable() &&
      in->hardlock.gather_set.empty()) {
    dout(7) << "inode_hard_eval finished gather on " << *in << endl;
    switch (in->hardlock.get_state()) {
    case LOCK_GLOCKR:
      in->hardlock.set_state(LOCK_LOCK);
      
      // waiters
      //in->hardlock.get_write();
      in->finish_waiting(CInode::WAIT_HARDRWB|CInode::WAIT_HARDSTABLE);
      //in->hardlock.put_write();
      break;
      
    default:
      assert(0);
    }
  }
  if (!in->hardlock.is_stable()) return;
  
  if (in->is_auth()) {

    // sync?
    if (in->is_replicated() &&
        in->is_hardlock_write_wanted() &&
        in->hardlock.get_state() != LOCK_SYNC) {
      dout(7) << "inode_hard_eval stable, syncing " << *in << endl;
      inode_hard_sync(in);
    }

  } else {
    // replica
  }
}


// mid

void Locker::inode_hard_sync(CInode *in)
{
  dout(7) << "inode_hard_sync on " << *in << endl;
  assert(in->is_auth());
  
  // check state
  if (in->hardlock.get_state() == LOCK_SYNC)
    return; // already sync
  if (in->hardlock.get_state() == LOCK_GLOCKR) 
    assert(0); // um... hmm!
  assert(in->hardlock.get_state() == LOCK_LOCK);
  
  // hard data
  bufferlist harddata;
  in->encode_hard_state(harddata);
  
  // bcast to replicas
  send_lock_message(in, LOCK_AC_SYNC, LOCK_OTYPE_IHARD, harddata);
  
  // change lock
  in->hardlock.set_state(LOCK_SYNC);
  
  // waiters?
  in->finish_waiting(CInode::WAIT_HARDSTABLE);
}

void Locker::inode_hard_lock(CInode *in)
{
  dout(7) << "inode_hard_lock on " << *in << " hardlock=" << in->hardlock << endl;  
  assert(in->is_auth());
  
  // check state
  if (in->hardlock.get_state() == LOCK_LOCK ||
      in->hardlock.get_state() == LOCK_GLOCKR) 
    return;  // already lock or locking
  assert(in->hardlock.get_state() == LOCK_SYNC);
  
  // bcast to replicas
  send_lock_message(in, LOCK_AC_LOCK, LOCK_OTYPE_IHARD);
  
  // change lock
  in->hardlock.set_state(LOCK_GLOCKR);
  in->hardlock.init_gather(in->get_replicas());
}





// messenger

void Locker::handle_lock_inode_hard(MLock *m)
{
  assert(m->get_otype() == LOCK_OTYPE_IHARD);
  
  if (mds->logger) mds->logger->inc("lih");

  int from = m->get_asker();
  CInode *in = mdcache->get_inode(m->get_ino());
  
  if (LOCK_AC_FOR_AUTH(m->get_action())) {
    // auth
    assert(in);
    assert(in->is_auth());// || in->is_proxy());
    dout(7) << "handle_lock_inode_hard " << *in << " hardlock=" << in->hardlock << endl;  

    /*if (in->is_proxy()) {
      // fw
      int newauth = in->authority().first;
      assert(newauth >= 0);
      if (from == newauth) {
        dout(7) << "handle_lock " << m->get_ino() << " from " << from << ": proxy, but from new auth, dropping" << endl;
        delete m;
      } else {
        dout(7) << "handle_lock " << m->get_ino() << " from " << from << ": proxy, fw to " << newauth << endl;
        mds->send_message_mds(m, newauth, MDS_PORT_LOCKER);
      }
      return;
    }
    */
  } else {
    // replica
    if (!in) {
      dout(7) << "handle_lock_inode_hard " << m->get_ino() << ": don't have it anymore" << endl;
      /* do NOT nak.. if we go that route we need to duplicate all the nonce funkiness
         to keep gather_set a proper/correct subset of cached_by.  better to use the existing
         cacheexpire mechanism instead!
      */
      delete m;
      return;
    }
    
    assert(!in->is_auth());
  }

  dout(7) << "handle_lock_inode_hard a=" << m->get_action() << " from " << from << " " << *in << " hardlock=" << in->hardlock << endl;  
 
  CLock *lock = &in->hardlock;
  
  switch (m->get_action()) {
    // -- replica --
  case LOCK_AC_SYNC:
    assert(lock->get_state() == LOCK_LOCK);
    
    { // assim data
      int off = 0;
      in->decode_hard_state(m->get_data(), off);
    }
    
    // update lock
    lock->set_state(LOCK_SYNC);
    
    // no need to reply
    
    // waiters
    in->finish_waiting(CInode::WAIT_HARDR|CInode::WAIT_HARDSTABLE);
    break;
    
  case LOCK_AC_LOCK:
    assert(lock->get_state() == LOCK_SYNC);
    //||           lock->get_state() == LOCK_GLOCKR);
    
    // wait for readers to finish?
    if (lock->get_nread() > 0) {
      dout(7) << "handle_lock_inode_hard readers, waiting before ack on " << *in << endl;
      lock->set_state(LOCK_GLOCKR);
      in->add_waiter(CInode::WAIT_HARDNORD,
                     new C_MDS_RetryMessage(mds, m));
      assert(0);  // does this ever happen?  (if so, fix hard_rdlock_finish, and CInodeExport.update_inode!)
      return;
     } else {

      // update lock and reply
      lock->set_state(LOCK_LOCK);
      
      {
        MLock *reply = new MLock(LOCK_AC_LOCKACK, mds->get_nodeid());
        reply->set_ino(in->ino(), LOCK_OTYPE_IHARD);
        mds->send_message_mds(reply, from, MDS_PORT_LOCKER);
      }
    }
    break;
    
    
    // -- auth --
  case LOCK_AC_LOCKACK:
    assert(lock->state == LOCK_GLOCKR);
    assert(lock->gather_set.count(from));
    lock->gather_set.erase(from);

    if (lock->gather_set.size()) {
      dout(7) << "handle_lock_inode_hard " << *in << " from " << from << ", still gathering " << lock->gather_set << endl;
    } else {
      dout(7) << "handle_lock_inode_hard " << *in << " from " << from << ", last one" << endl;
      inode_hard_eval(in);
    }
  }  
  delete m;
}




// =====================
// soft inode metadata


bool Locker::inode_file_rdlock_start(CInode *in, MDRequest *mdr)
{
  dout(7) << "inode_file_rdlock_start " << *in << " filelock=" << in->filelock << endl;  

  // can read?  grab ref.
  if (in->filelock.can_read(in->is_auth())) {
    in->filelock.get_read();
    return true;
  }
  
  // can't read, and replicated.
  if (in->filelock.can_read_soon(in->is_auth())) {
    // wait
    dout(7) << "inode_file_rdlock_start can_read_soon " << *in << endl;
  } else {    
    if (in->is_auth()) {
      // auth

      // FIXME or qsync?

      if (in->filelock.is_stable()) {
        inode_file_lock(in);     // lock, bc easiest to back off

        if (in->filelock.can_read(in->is_auth())) {
          in->filelock.get_read();
          
          //in->filelock.get_write();
          in->finish_waiting(CInode::WAIT_FILERWB|CInode::WAIT_FILESTABLE);
          //in->filelock.put_write();

	  mdr->inode_file_rdlocks.insert(in);
	  mdr->inode_file_locks.insert(in);
          return true;
        }
      } else {
        dout(7) << "inode_file_rdlock_start waiting until stable on " << *in << ", filelock=" << in->filelock << endl;
        in->add_waiter(CInode::WAIT_FILESTABLE, new C_MDS_RetryRequest(mdcache, mdr));
        return false;
      }
    } else {
      // replica
      if (in->filelock.is_stable()) {

        // fw to auth
        int auth = in->authority().first;
        dout(7) << "inode_file_rdlock_start " << *in << " on replica and async, fw to auth " << auth << endl;
        assert(auth != mds->get_nodeid());
        mdcache->request_forward(mdr, auth);
        return false;
        
      } else {
        // wait until stable
        dout(7) << "inode_file_rdlock_start waiting until stable on " << *in << ", filelock=" << in->filelock << endl;
        in->add_waiter(CInode::WAIT_FILESTABLE, new C_MDS_RetryRequest(mdcache, mdr));
        return false;
      }
    }
  }

  // wait
  dout(7) << "inode_file_rdlock_start waiting on " << *in << ", filelock=" << in->filelock << endl;
  in->add_waiter(CInode::WAIT_FILER, new C_MDS_RetryRequest(mdcache, mdr));
        
  return false;
}


void Locker::inode_file_rdlock_finish(CInode *in, MDRequest *mdr)
{
  // drop ref
  assert(in->filelock.can_read(in->is_auth()));
  in->filelock.put_read();
  mdr->inode_file_rdlocks.erase(in);
  mdr->inode_file_locks.erase(in);

  dout(7) << "inode_file_rdlock_finish on " << *in << ", filelock=" << in->filelock << endl;

  if (in->filelock.get_nread() == 0) {
    in->finish_waiting(CInode::WAIT_FILENORD);
    inode_file_eval(in);
  }
}


bool Locker::inode_file_xlock_start(CInode *in, MDRequest *mdr)
{
  dout(7) << "inode_file_xlock_start on " << *in << endl;

  // can't write?
  if (!in->filelock.can_write(in->is_auth())) {
  
    // can't write.
    if (in->is_auth()) {
      // auth
      if (!in->filelock.can_write_soon(in->is_auth())) {
	if (!in->filelock.is_stable()) {
	  dout(7) << "inode_file_xlock_start on auth, waiting for stable on " << *in << endl;
	  in->add_waiter(CInode::WAIT_FILESTABLE, new C_MDS_RetryRequest(mdcache, mdr));
	  return false;
	}
	
	// initiate lock 
	inode_file_lock(in);

	// fall-thru to below.
      }
    } else {
      // replica
      // fw to auth
      int auth = in->authority().first;
      dout(7) << "inode_file_xlock_start " << *in << " on replica, fw to auth " << auth << endl;
      assert(auth != mds->get_nodeid());
      mdcache->request_forward(mdr, auth);
      return false;
    }
  } 
  
  // check again
  if (in->filelock.can_write(in->is_auth())) {
    // can i auth pin?
    assert(in->is_auth());
    in->filelock.get_write(mdr);
    mdr->inode_file_locks.insert(in);
    mdr->inode_file_xlocks.insert(in);
    return true;
  } else {
    dout(7) << "inode_file_xlock_start on auth, waiting for write on " << *in << endl;
    in->add_waiter(CInode::WAIT_FILEW, new C_MDS_RetryRequest(mdcache, mdr));
    return false;
  }
}


void Locker::inode_file_xlock_finish(CInode *in, MDRequest *mdr)
{
  // drop ref
  //assert(in->filelock.can_write(in->is_auth()));
  in->filelock.put_write();
  mdr->inode_file_locks.erase(in);
  mdr->inode_file_xlocks.erase(in);
  dout(7) << "inode_file_xlock_finish on " << *in << ", filelock=" << in->filelock << endl;
  
  // drop lock?
  if (!in->is_filelock_write_wanted()) {
    in->finish_waiting(CInode::WAIT_FILENOWR);
    inode_file_eval(in);
  }
}


/*
 * ...
 *
 * also called after client caps are acked to us
 * - checks if we're in unstable sfot state and can now move on to next state
 * - checks if soft state should change (eg bc last writer closed)
 */

void Locker::inode_file_eval(CInode *in)
{
  int issued = in->get_caps_issued();

  // [auth] finished gather?
  if (in->is_auth() &&
      !in->filelock.is_stable() &&
      in->filelock.gather_set.size() == 0) {
    dout(7) << "inode_file_eval finished mds gather on " << *in << endl;

    switch (in->filelock.get_state()) {
      // to lock
    case LOCK_GLOCKR:
    case LOCK_GLOCKM:
    case LOCK_GLOCKL:
      if (issued == 0) {
        in->filelock.set_state(LOCK_LOCK);
        
        // waiters
        in->filelock.get_read();
        //in->filelock.get_write();
        in->finish_waiting(CInode::WAIT_FILERWB|CInode::WAIT_FILESTABLE);
        in->filelock.put_read();
        //in->filelock.put_write();
      }
      break;
      
      // to mixed
    case LOCK_GMIXEDR:
      if ((issued & ~(CAP_FILE_RD)) == 0) {
        in->filelock.set_state(LOCK_MIXED);
        in->finish_waiting(CInode::WAIT_FILESTABLE);
      }
      break;

    case LOCK_GMIXEDL:
      if ((issued & ~(CAP_FILE_WR)) == 0) {
        in->filelock.set_state(LOCK_MIXED);

        if (in->is_replicated()) {
          // data
          bufferlist softdata;
          in->encode_file_state(softdata);
          
          // bcast to replicas
	  send_lock_message(in, LOCK_AC_MIXED, LOCK_OTYPE_IFILE, softdata);
        }

        in->finish_waiting(CInode::WAIT_FILESTABLE);
      }
      break;

      // to loner
    case LOCK_GLONERR:
      if (issued == 0) {
        in->filelock.set_state(LOCK_LONER);
        in->finish_waiting(CInode::WAIT_FILESTABLE);
      }
      break;

    case LOCK_GLONERM:
      if ((issued & ~CAP_FILE_WR) == 0) {
        in->filelock.set_state(LOCK_LONER);
        in->finish_waiting(CInode::WAIT_FILESTABLE);
      }
      break;
      
      // to sync
    case LOCK_GSYNCL:
    case LOCK_GSYNCM:
      if ((issued & ~(CAP_FILE_RD)) == 0) {
        in->filelock.set_state(LOCK_SYNC);
        
        { // bcast data to replicas
          bufferlist softdata;
          in->encode_file_state(softdata);
          
	  send_lock_message(in, LOCK_AC_SYNC, LOCK_OTYPE_IFILE, softdata);
        }
        
        // waiters
        in->filelock.get_read();
        in->finish_waiting(CInode::WAIT_FILER|CInode::WAIT_FILESTABLE);
        in->filelock.put_read();
      }
      break;
      
    default: 
      assert(0);
    }

    issue_caps(in);
  }
  
  // [replica] finished caps gather?
  if (!in->is_auth() &&
      !in->filelock.is_stable()) {
    switch (in->filelock.get_state()) {
    case LOCK_GMIXEDR:
      if ((issued & ~(CAP_FILE_RD)) == 0) {
        in->filelock.set_state(LOCK_MIXED);
        
        // ack
        MLock *reply = new MLock(LOCK_AC_MIXEDACK, mds->get_nodeid());
        reply->set_ino(in->ino(), LOCK_OTYPE_IFILE);
        mds->send_message_mds(reply, in->authority().first, MDS_PORT_LOCKER);
      }
      break;

    case LOCK_GLOCKR:
      if (issued == 0) {
        in->filelock.set_state(LOCK_LOCK);
        
        // ack
        MLock *reply = new MLock(LOCK_AC_LOCKACK, mds->get_nodeid());
        reply->set_ino(in->ino(), LOCK_OTYPE_IFILE);
        mds->send_message_mds(reply, in->authority().first, MDS_PORT_LOCKER);
      }
      break;

    default:
      assert(0);
    }
  }

  // !stable -> do nothing.
  if (!in->filelock.is_stable()) return; 


  // stable.
  assert(in->filelock.is_stable());

  if (in->is_auth()) {
    // [auth]
    int wanted = in->get_caps_wanted();
    bool loner = (in->client_caps.size() == 1) && in->mds_caps_wanted.empty();
    dout(7) << "inode_file_eval wanted=" << cap_string(wanted)
            << "  filelock=" << in->filelock 
            << "  loner=" << loner
            << endl;

    // * -> loner?
    if (in->filelock.get_nread() == 0 &&
        !in->is_filelock_write_wanted() &&
        (wanted & CAP_FILE_WR) &&
        loner &&
        in->filelock.get_state() != LOCK_LONER) {
      dout(7) << "inode_file_eval stable, bump to loner " << *in << ", filelock=" << in->filelock << endl;
      inode_file_loner(in);
    }

    // * -> mixed?
    else if (in->filelock.get_nread() == 0 &&
             !in->is_filelock_write_wanted() &&
             (wanted & CAP_FILE_RD) &&
             (wanted & CAP_FILE_WR) &&
             !(loner && in->filelock.get_state() == LOCK_LONER) &&
             in->filelock.get_state() != LOCK_MIXED) {
      dout(7) << "inode_file_eval stable, bump to mixed " << *in << ", filelock=" << in->filelock << endl;
      inode_file_mixed(in);
    }

    // * -> sync?
    else if (!in->is_filelock_write_wanted() &&
             !(wanted & CAP_FILE_WR) &&
             ((wanted & CAP_FILE_RD) || 
              in->is_replicated() || 
              (!loner && in->filelock.get_state() == LOCK_LONER)) &&
             in->filelock.get_state() != LOCK_SYNC) {
      dout(7) << "inode_file_eval stable, bump to sync " << *in << ", filelock=" << in->filelock << endl;
      inode_file_sync(in);
    }

    // * -> lock?  (if not replicated or open)
    else if (!in->is_replicated() &&
             wanted == 0 &&
             in->filelock.get_state() != LOCK_LOCK) {
      inode_file_lock(in);
    }
    
  } else {
    // replica
    // recall? check wiaters?  XXX
  }
}


// mid

bool Locker::inode_file_sync(CInode *in)
{
  dout(7) << "inode_file_sync " << *in << " filelock=" << in->filelock << endl;  

  assert(in->is_auth());

  // check state
  if (in->filelock.get_state() == LOCK_SYNC ||
      in->filelock.get_state() == LOCK_GSYNCL ||
      in->filelock.get_state() == LOCK_GSYNCM)
    return true;

  assert(in->filelock.is_stable());

  int issued = in->get_caps_issued();

  assert((in->get_caps_wanted() & CAP_FILE_WR) == 0);

  if (in->filelock.get_state() == LOCK_LOCK) {
    if (in->is_replicated()) {
      // soft data
      bufferlist softdata;
      in->encode_file_state(softdata);
      
      // bcast to replicas
      send_lock_message(in, LOCK_AC_SYNC, LOCK_OTYPE_IFILE, softdata);
    }

    // change lock
    in->filelock.set_state(LOCK_SYNC);

    // reissue caps
    issue_caps(in);
    return true;
  }

  else if (in->filelock.get_state() == LOCK_MIXED) {
    // writers?
    if (issued & CAP_FILE_WR) {
      // gather client write caps
      in->filelock.set_state(LOCK_GSYNCM);
      issue_caps(in);
    } else {
      // no writers, go straight to sync

      if (in->is_replicated()) {
        // bcast to replicas
	send_lock_message(in, LOCK_AC_SYNC, LOCK_OTYPE_IFILE);
      }
    
      // change lock
      in->filelock.set_state(LOCK_SYNC);
    }
    return false;
  }

  else if (in->filelock.get_state() == LOCK_LONER) {
    // writers?
    if (issued & CAP_FILE_WR) {
      // gather client write caps
      in->filelock.set_state(LOCK_GSYNCL);
      issue_caps(in);
    } else {
      // no writers, go straight to sync
      if (in->is_replicated()) {
        // bcast to replicas
	send_lock_message(in, LOCK_AC_SYNC, LOCK_OTYPE_IFILE);
      }

      // change lock
      in->filelock.set_state(LOCK_SYNC);
    }
    return false;
  }
  else 
    assert(0); // wtf.

  return false;
}



void Locker::inode_file_lock(CInode *in)
{
  dout(7) << "inode_file_lock " << *in << " filelock=" << in->filelock << endl;  

  assert(in->is_auth());
  
  // check state
  if (in->filelock.get_state() == LOCK_LOCK ||
      in->filelock.get_state() == LOCK_GLOCKR ||
      in->filelock.get_state() == LOCK_GLOCKM ||
      in->filelock.get_state() == LOCK_GLOCKL) 
    return;  // lock or locking

  assert(in->filelock.is_stable());

  int issued = in->get_caps_issued();

  if (in->filelock.get_state() == LOCK_SYNC) {
    if (in->is_replicated()) {
      // bcast to replicas
      send_lock_message(in, LOCK_AC_LOCK, LOCK_OTYPE_IFILE);
      in->filelock.init_gather(in->get_replicas());
      
      // change lock
      in->filelock.set_state(LOCK_GLOCKR);

      // call back caps
      if (issued) 
        issue_caps(in);
    } else {
      if (issued) {
        // call back caps
        in->filelock.set_state(LOCK_GLOCKR);
        issue_caps(in);
      } else {
        in->filelock.set_state(LOCK_LOCK);
      }
    }
  }

  else if (in->filelock.get_state() == LOCK_MIXED) {
    if (in->is_replicated()) {
      // bcast to replicas
      send_lock_message(in, LOCK_AC_LOCK, LOCK_OTYPE_IFILE);
      in->filelock.init_gather(in->get_replicas());

      // change lock
      in->filelock.set_state(LOCK_GLOCKM);
      
      // call back caps
      issue_caps(in);
    } else {
      //assert(issued);  // ??? -sage 2/19/06
      if (issued) {
        // change lock
        in->filelock.set_state(LOCK_GLOCKM);
        
        // call back caps
        issue_caps(in);
      } else {
        in->filelock.set_state(LOCK_LOCK);
      }
    }
      
  }
  else if (in->filelock.get_state() == LOCK_LONER) {
    if (issued & CAP_FILE_WR) {
      // change lock
      in->filelock.set_state(LOCK_GLOCKL);
  
      // call back caps
      issue_caps(in);
    } else {
      in->filelock.set_state(LOCK_LOCK);
    }
  }
  else 
    assert(0); // wtf.
}


void Locker::inode_file_mixed(CInode *in)
{
  dout(7) << "inode_file_mixed " << *in << " filelock=" << in->filelock << endl;  

  assert(in->is_auth());
  
  // check state
  if (in->filelock.get_state() == LOCK_GMIXEDR ||
      in->filelock.get_state() == LOCK_GMIXEDL)
    return;     // mixed or mixing

  assert(in->filelock.is_stable());

  int issued = in->get_caps_issued();

  if (in->filelock.get_state() == LOCK_SYNC) {
    if (in->is_replicated()) {
      // bcast to replicas
      send_lock_message(in, LOCK_AC_MIXED, LOCK_OTYPE_IFILE);
      in->filelock.init_gather(in->get_replicas());
    
      in->filelock.set_state(LOCK_GMIXEDR);
      issue_caps(in);
    } else {
      if (issued) {
        in->filelock.set_state(LOCK_GMIXEDR);
        issue_caps(in);
      } else {
        in->filelock.set_state(LOCK_MIXED);
      }
    }
  }

  else if (in->filelock.get_state() == LOCK_LOCK) {
    if (in->is_replicated()) {
      // data
      bufferlist softdata;
      in->encode_file_state(softdata);
      
      // bcast to replicas
      send_lock_message(in, LOCK_AC_MIXED, LOCK_OTYPE_IFILE, softdata);
    }

    // change lock
    in->filelock.set_state(LOCK_MIXED);
    issue_caps(in);
  }

  else if (in->filelock.get_state() == LOCK_LONER) {
    if (issued & CAP_FILE_WRBUFFER) {
      // gather up WRBUFFER caps
      in->filelock.set_state(LOCK_GMIXEDL);
      issue_caps(in);
    }
    else if (in->is_replicated()) {
      // bcast to replicas
      send_lock_message(in, LOCK_AC_MIXED, LOCK_OTYPE_IFILE);
      in->filelock.set_state(LOCK_MIXED);
      issue_caps(in);
    } else {
      in->filelock.set_state(LOCK_MIXED);
      issue_caps(in);
    }
  }

  else 
    assert(0); // wtf.
}


void Locker::inode_file_loner(CInode *in)
{
  dout(7) << "inode_file_loner " << *in << " filelock=" << in->filelock << endl;  

  assert(in->is_auth());

  // check state
  if (in->filelock.get_state() == LOCK_LONER ||
      in->filelock.get_state() == LOCK_GLONERR ||
      in->filelock.get_state() == LOCK_GLONERM)
    return; 

  assert(in->filelock.is_stable());
  assert((in->client_caps.size() == 1) && in->mds_caps_wanted.empty());
  
  if (in->filelock.get_state() == LOCK_SYNC) {
    if (in->is_replicated()) {
      // bcast to replicas
      send_lock_message(in, LOCK_AC_LOCK, LOCK_OTYPE_IFILE);
      in->filelock.init_gather(in->get_replicas());
      
      // change lock
      in->filelock.set_state(LOCK_GLONERR);
    } else {
      // only one guy with file open, who gets it all, so
      in->filelock.set_state(LOCK_LONER);
      issue_caps(in);
    }
  }

  else if (in->filelock.get_state() == LOCK_LOCK) {
    // change lock.  ignore replicas; they don't know about LONER.
    in->filelock.set_state(LOCK_LONER);
    issue_caps(in);
  }

  else if (in->filelock.get_state() == LOCK_MIXED) {
    if (in->is_replicated()) {
      // bcast to replicas
      send_lock_message(in, LOCK_AC_LOCK, LOCK_OTYPE_IFILE);
      in->filelock.init_gather(in->get_replicas());
      
      // change lock
      in->filelock.set_state(LOCK_GLONERM);
    } else {
      in->filelock.set_state(LOCK_LONER);
      issue_caps(in);
    }
  }

  else 
    assert(0);
}

// messenger

void Locker::handle_lock_inode_file(MLock *m)
{
  assert(m->get_otype() == LOCK_OTYPE_IFILE);
  
  if (mds->logger) mds->logger->inc("lif");

  CInode *in = mdcache->get_inode(m->get_ino());
  int from = m->get_asker();

  if (LOCK_AC_FOR_AUTH(m->get_action())) {
    // auth
    assert(in);
    assert(in->is_auth());// || in->is_proxy());
    dout(7) << "handle_lock_inode_file " << *in << " hardlock=" << in->hardlock << endl;  
        
    /*if (in->is_proxy()) {
      // fw
      int newauth = in->authority().first;
      assert(newauth >= 0);
      if (from == newauth) {
        dout(7) << "handle_lock " << m->get_ino() << " from " << from << ": proxy, but from new auth, dropping" << endl;
        delete m;
      } else {
        dout(7) << "handle_lock " << m->get_ino() << " from " << from << ": proxy, fw to " << newauth << endl;
        mds->send_message_mds(m, newauth, MDS_PORT_LOCKER);
      }
      return;
    }
    */
  } else {
    // replica
    if (!in) {
      // drop it.  don't nak.
      dout(7) << "handle_lock " << m->get_ino() << ": don't have it anymore" << endl;
      delete m;
      return;
    }
    
    assert(!in->is_auth());
  }

  dout(7) << "handle_lock_inode_file a=" << m->get_action() << " from " << from << " " << *in << " filelock=" << in->filelock << endl;  
  
  CLock *lock = &in->filelock;
  int issued = in->get_caps_issued();

  switch (m->get_action()) {
    // -- replica --
  case LOCK_AC_SYNC:
    assert(lock->get_state() == LOCK_LOCK ||
           lock->get_state() == LOCK_MIXED);
    
    { // assim data
      int off = 0;
      in->decode_file_state(m->get_data(), off);
    }
    
    // update lock
    lock->set_state(LOCK_SYNC);
    
    // no need to reply.
    
    // waiters
    in->filelock.get_read();
    in->finish_waiting(CInode::WAIT_FILER|CInode::WAIT_FILESTABLE);
    in->filelock.put_read();
    inode_file_eval(in);
    break;
    
  case LOCK_AC_LOCK:
    assert(lock->get_state() == LOCK_SYNC ||
           lock->get_state() == LOCK_MIXED);
    
    // call back caps?
    if (issued & CAP_FILE_RD) {
      dout(7) << "handle_lock_inode_file client readers, gathering caps on " << *in << endl;
      issue_caps(in);
    }
    if (lock->get_nread() > 0) {
      dout(7) << "handle_lock_inode_file readers, waiting before ack on " << *in << endl;
      in->add_waiter(CInode::WAIT_FILENORD,
                     new C_MDS_RetryMessage(mds,m));
      lock->set_state(LOCK_GLOCKR);
      assert(0);// i am broken.. why retry message when state captures all the info i need?
      return;
    } 
    if (issued & CAP_FILE_RD) {
      lock->set_state(LOCK_GLOCKR);
      break;
    }

    // nothing to wait for, lock and ack.
    {
      lock->set_state(LOCK_LOCK);

      MLock *reply = new MLock(LOCK_AC_LOCKACK, mds->get_nodeid());
      reply->set_ino(in->ino(), LOCK_OTYPE_IFILE);
      mds->send_message_mds(reply, from, MDS_PORT_LOCKER);
    }
    break;
    
  case LOCK_AC_MIXED:
    assert(lock->get_state() == LOCK_SYNC ||
           lock->get_state() == LOCK_LOCK);
    
    if (lock->get_state() == LOCK_SYNC) {
      // MIXED
      if (issued & CAP_FILE_RD) {
        // call back client caps
        lock->set_state(LOCK_GMIXEDR);
        issue_caps(in);
        break;
      } else {
        // no clients, go straight to mixed
        lock->set_state(LOCK_MIXED);

        // ack
        MLock *reply = new MLock(LOCK_AC_MIXEDACK, mds->get_nodeid());
        reply->set_ino(in->ino(), LOCK_OTYPE_IFILE);
        mds->send_message_mds(reply, from, MDS_PORT_LOCKER);
      }
    } else {
      // LOCK
      lock->set_state(LOCK_MIXED);
      
      // no ack needed.
    }

    issue_caps(in);
    
    // waiters
    //in->filelock.get_write();
    in->finish_waiting(CInode::WAIT_FILEW|CInode::WAIT_FILESTABLE);
    //in->filelock.put_write();
    inode_file_eval(in);
    break;

 
    

    // -- auth --
  case LOCK_AC_LOCKACK:
    assert(lock->state == LOCK_GLOCKR ||
           lock->state == LOCK_GLOCKM ||
           lock->state == LOCK_GLONERM ||
           lock->state == LOCK_GLONERR);
    assert(lock->gather_set.count(from));
    lock->gather_set.erase(from);

    if (lock->gather_set.size()) {
      dout(7) << "handle_lock_inode_file " << *in << " from " << from << ", still gathering " << lock->gather_set << endl;
    } else {
      dout(7) << "handle_lock_inode_file " << *in << " from " << from << ", last one" << endl;
      inode_file_eval(in);
    }
    break;
    
  case LOCK_AC_SYNCACK:
    assert(lock->state == LOCK_GSYNCM);
    assert(lock->gather_set.count(from));
    lock->gather_set.erase(from);
    
    /* not used currently
    {
      // merge data  (keep largest size, mtime, etc.)
      int off = 0;
      in->decode_merge_file_state(m->get_data(), off);
    }
    */

    if (lock->gather_set.size()) {
      dout(7) << "handle_lock_inode_file " << *in << " from " << from << ", still gathering " << lock->gather_set << endl;
    } else {
      dout(7) << "handle_lock_inode_file " << *in << " from " << from << ", last one" << endl;
      inode_file_eval(in);
    }
    break;

  case LOCK_AC_MIXEDACK:
    assert(lock->state == LOCK_GMIXEDR);
    assert(lock->gather_set.count(from));
    lock->gather_set.erase(from);
    
    if (lock->gather_set.size()) {
      dout(7) << "handle_lock_inode_file " << *in << " from " << from << ", still gathering " << lock->gather_set << endl;
    } else {
      dout(7) << "handle_lock_inode_file " << *in << " from " << from << ", last one" << endl;
      inode_file_eval(in);
    }
    break;


  default:
    assert(0);
  }  
  
  delete m;
}














void Locker::handle_lock_dir(MLock *m) 
{
}



// DENTRY


// trace helpers

/** dentry_can_rdlock_trace
 * see if we can _anonymously_ rdlock an entire trace.  
 * if not, and req is specified, wait and retry that message.
 */
bool Locker::dentry_can_rdlock_trace(vector<CDentry*>& trace, MClientRequest *req) 
{
  // verify dentries are rdlockable.
  // we do this because
  // - we're being less aggressive about locks acquisition, and
  // - we're not acquiring the locks in order!
  for (vector<CDentry*>::iterator it = trace.begin();
       it != trace.end();
       it++) {
    CDentry *dn = *it;
    if (!dn->is_pinnable(0)) {
      if (req) {
	dout(10) << "can_rdlock_trace can't rdlock " << *dn << ", waiting" << endl;
	dn->dir->add_waiter(CDir::WAIT_DNPINNABLE,   
			    dn->name,
			    new C_MDS_RetryMessage(mds, req));
      } else {
	dout(10) << "can_rdlock_trace can't rdlock " << *dn << endl;
      }
      return false;
    }
  }
  return true;
}

void Locker::dentry_anon_rdlock_trace_start(vector<CDentry*>& trace)
{
  // grab dentry rdlocks
  for (vector<CDentry*>::iterator it = trace.begin();
       it != trace.end();
       it++)
    (*it)->pin(0);
}



bool Locker::dentry_rdlock_start(CDentry *dn, MDRequest *mdr)
{
  // verify lockable
  if (!dn->is_pinnable(mdr)) {
    // wait
    dout(10) << "dentry_rdlock_start waiting on " << *dn << endl;
    dn->dir->add_waiter(CDir::WAIT_DNPINNABLE,   
			dn->name,
			new C_MDS_RetryRequest(mdcache, mdr));
    return false;
  }

  // rdlock
  dout(10) << "dentry_rdlock_start " << *dn << endl;
  dn->pin(mdr);

  mdr->dentry_rdlocks.insert(dn);
  mdr->dentry_locks.insert(dn);

  return true;
}


void Locker::_dentry_rdlock_finish(CDentry *dn, MDRequest *mdr)
{
  dn->unpin(mdr);

  // did we completely unpin a waiter?
  if (dn->lockstate == DN_LOCK_UNPINNING && !dn->get_num_ref()) {
    // return state to sync, in case the unpinner flails
    dn->lockstate = DN_LOCK_SYNC;
    
    // run finisher right now to give them a fair shot.
    dn->dir->finish_waiting(CDir::WAIT_DNUNPINNED, dn->name);
  }
}

void Locker::dentry_rdlock_finish(CDentry *dn, MDRequest *mdr)
{
  dout(10) << "dentry_rdlock_finish " << *dn << endl;
  _dentry_rdlock_finish(dn, mdr);
  mdr->dentry_rdlocks.erase(dn);
  mdr->dentry_locks.erase(dn);
}

void Locker::dentry_anon_rdlock_trace_finish(vector<CDentry*>& trace)
{
  for (vector<CDentry*>::iterator it = trace.begin();
       it != trace.end();
       it++) 
    _dentry_rdlock_finish(*it, 0);
}

bool Locker::dentry_xlock_start(CDentry *dn, MDRequest *mdr)
{
  dout(7) << "dentry_xlock_start on " << *dn << endl;

  // locked?
  if (dn->lockstate == DN_LOCK_XLOCK) {
    if (dn->xlockedby == mdr) return true;  // locked by me!

    // not by me, wait
    dout(7) << "dentry " << *dn << " xlock by someone else" << endl;
    dn->dir->add_waiter(CDir::WAIT_DNREAD, dn->name,
                        new C_MDS_RetryRequest(mdcache, mdr));
    return false;
  }

  // prelock?
  if (dn->lockstate == DN_LOCK_PREXLOCK) {
    if (dn->xlockedby == mdr) {
      dout(7) << "dentry " << *dn << " prexlock by me" << endl;
      dn->dir->add_waiter(CDir::WAIT_DNLOCK, dn->name,
                          new C_MDS_RetryRequest(mdcache, mdr));
    } else {
      dout(7) << "dentry " << *dn << " prexlock by someone else" << endl;
      dn->dir->add_waiter(CDir::WAIT_DNREAD, dn->name,
                          new C_MDS_RetryRequest(mdcache, mdr));
    }
    return false;
  }


  // lockable!
  assert(dn->lockstate == DN_LOCK_SYNC ||
         dn->lockstate == DN_LOCK_UNPINNING);
  
  // is dentry path pinned?
  if (dn->is_pinned()) {
    dout(7) << "dentry " << *dn << " pinned, waiting" << endl;
    dn->lockstate = DN_LOCK_UNPINNING;
    dn->dir->add_waiter(CDir::WAIT_DNUNPINNED,
                        dn->name,
                        new C_MDS_RetryRequest(mdcache, mdr));
    return false;
  }

  // mine!
  dn->xlockedby = mdr;
  
  // pin me!
  dn->get(CDentry::PIN_XLOCK);

  if (dn->is_replicated()) {
    dn->lockstate = DN_LOCK_PREXLOCK;
    
    // xlock with whom?
    set<int> who;
    for (map<int,int>::iterator p = dn->replicas_begin();
	 p != dn->replicas_end();
	 ++p)
      who.insert(p->first);
    dn->gather_set = who;

    // make path
    string path;
    dn->make_path(path);
    dout(10) << "path is " << path << " for " << *dn << endl;

    for (set<int>::iterator it = who.begin();
         it != who.end();
         it++) {
      MLock *m = new MLock(LOCK_AC_LOCK, mds->get_nodeid());
      m->set_dn(dn->dir->dirfrag(), dn->name);
      m->set_path(path);
      mds->send_message_mds(m, *it, MDS_PORT_LOCKER);
    }

    // wait
    dout(7) << "dentry_xlock_start locking, waiting for replicas " << endl;
    dn->dir->add_waiter(CDir::WAIT_DNLOCK, dn->name,
                        new C_MDS_RetryRequest(mdcache, mdr));
    return false;
  } else {
    dn->lockstate = DN_LOCK_XLOCK;
    mdr->dentry_xlocks.insert(dn);
    mdr->dentry_locks.insert(dn);
    return true;
  }
}

void Locker::dentry_xlock_finish(CDentry *dn, MDRequest *mdr, bool quiet)
{
  dout(7) << "dentry_xlock_finish on " << *dn << endl;
  
  assert(dn->xlockedby);
  if (dn->xlockedby == DN_XLOCK_FOREIGN) {
    dout(7) << "this was a foreign xlock" << endl;
  } else {
    // remove from request record
    mdr->dentry_xlocks.erase(dn);
    mdr->dentry_locks.erase(dn);
  }

  dn->xlockedby = 0;
  dn->lockstate = DN_LOCK_SYNC;

  // unpin
  dn->put(CDentry::PIN_XLOCK);

  // tell replicas?
  if (!quiet) {
    // tell even if dn is null.
    if (dn->is_replicated()) {
      send_lock_message(dn, LOCK_AC_SYNC);
    }
  }
  
  // kick waiters
  list<Context*> finished;
  dn->dir->take_waiting(CDir::WAIT_DNREAD, finished);
  mds->queue_finished(finished);
}


void Locker::dentry_xlock_downgrade_to_rdlock(CDentry *dn, MDRequest *mdr)
{
  dout(7) << "dentry_xlock_downgrade_to_rdlock on " << *dn << endl;
  
  assert(dn->xlockedby);
  if (dn->xlockedby == DN_XLOCK_FOREIGN) {
    dout(7) << "this was a foreign xlock" << endl;
    assert(0); // rewrite me
  }

  // un-xlock
  dn->xlockedby = 0;
  dn->lockstate = DN_LOCK_SYNC;
  mdr->dentry_xlocks.erase(dn);
  dn->put(CDentry::PIN_XLOCK);

  // rdlock
  mdr->dentry_rdlocks.insert(dn);
  dn->pin(mdr);

  // tell replicas?
  if (dn->is_replicated()) {
    send_lock_message(dn, LOCK_AC_SYNC);
  }
  
  // kick waiters
  list<Context*> finished;
  dn->dir->take_waiting(CDir::WAIT_DNREAD, finished);
  mds->queue_finished(finished);
}


/*
 * onfinish->finish() will be called with 
 * 0 on successful xlock,
 * -1 on failure
 */
/*
class C_MDC_XlockRequest : public Context {
  Locker *mdc;
  CDir *dir;
  string dname;
  Message *req;
  Context *finisher;
public:
  C_MDC_XlockRequest(Locker *mdc, 
                     CDir *dir, const string& dname, 
                     Message *req,
                     Context *finisher) {
    this->mdc = mdc;
    this->dir = dir;
    this->dname = dname;
    this->req = req;
    this->finisher = finisher;
  }

  void finish(int r) {
    mdc->dentry_xlock_request_finish(r, dir, dname, req, finisher);
  }
};

void Locker::dentry_xlock_request_finish(int r, 
					  CDir *dir, const string& dname, 
					  Message *req,
					  Context *finisher) 
{
  dout(10) << "dentry_xlock_request_finish r = " << r << endl;
  if (r == 1) {  // 1 for xlock request success
    CDentry *dn = dir->lookup(dname);
    if (dn && dn->xlockedby == 0) {
      // success
      dn->xlockedby = req;   // our request was the winner
      dout(10) << "xlock request success, now xlocked by req " << req << " dn " << *dn << endl;
      
      // remember!
      mdcache->active_requests[req].foreign_xlocks.insert(dn);
    }        
  }
  
  // retry request (or whatever)
  finisher->finish(0);
  delete finisher;
}

void Locker::dentry_xlock_request(CDir *dir, const string& dname, bool create,
                                   Message *req, Context *onfinish)
{
  dout(10) << "dentry_xlock_request on dn " << dname << " create=" << create << " in " << *dir << endl; 
  // send request
  int dauth = dir->dentry_authority(dname).first;
  MLock *m = new MLock(create ? LOCK_AC_REQXLOCKC:LOCK_AC_REQXLOCK, mds->get_nodeid());
  m->set_dn(dir->dirfrag(), dname);
  mds->send_message_mds(m, dauth, MDS_PORT_LOCKER);
  
  // add waiter
  dir->add_waiter(CDir::WAIT_DNREQXLOCK, dname, 
                  new C_MDC_XlockRequest(this, 
                                         dir, dname, req,
                                         onfinish));
}
*/



void Locker::handle_lock_dn(MLock *m)
{
  assert(m->get_otype() == LOCK_OTYPE_DN);
  
  CDir *dir = mdcache->get_dirfrag(m->get_dirfrag());  // may be null 
  string dname = m->get_dn();
  int from = m->get_asker();
  CDentry *dn = 0;

  if (LOCK_AC_FOR_AUTH(m->get_action())) {
    // auth

    // normally we have it always
    if (dir) {
      int dauth = dir->dentry_authority(dname).first;
      assert(dauth == mds->get_nodeid() || dir->is_proxy() ||  // mine or proxy,
             m->get_action() == LOCK_AC_REQXLOCKACK ||         // or we did a REQXLOCK and this is our ack/nak
             m->get_action() == LOCK_AC_REQXLOCKNAK);
      
      if (dir->is_proxy()) {

        assert(dauth >= 0);

        if (dauth == m->get_asker() && 
            (m->get_action() == LOCK_AC_REQXLOCK ||
             m->get_action() == LOCK_AC_REQXLOCKC)) {
          dout(7) << "handle_lock_dn got reqxlock from " << dauth << " and they are auth.. dropping on floor (their import will have woken them up)" << endl;
          /*if (mdcache->active_requests.count(m)) 
            mdcache->request_finish(m);
          else
            delete m;
	  */
	  assert(0); // FIXME REWRITE ME >>>>>>>
          return;
        }

        dout(7) << "handle_lock_dn " << m << " " << m->get_ino() << " dname " << dname << " from " << from << ": proxy, fw to " << dauth << endl;

	/* ******* REWRITE ME SDFKJDSFDSFJK:SDFJKDFSJKFDSHJKDFSHJKDFS>>>>>>>
        // forward
        if (mdcache->active_requests.count(m)) {
          // xlock requests are requests, use request_* functions!
          assert(m->get_action() == LOCK_AC_REQXLOCK ||
                 m->get_action() == LOCK_AC_REQXLOCKC);
          // forward as a request
          mdcache->request_forward(m, dauth, MDS_PORT_LOCKER);
        } else {
          // not an xlock req, or it is and we just didn't register the request yet
          // forward normally
          mds->send_message_mds(m, dauth, MDS_PORT_LOCKER);
        }
	*/
        return;
      }
      
      dn = dir->lookup(dname);
    }

    // except with.. an xlock request?
    if (!dn) {
      assert(dir);  // we should still have the dir, though!  the requester has the dir open.
      switch (m->get_action()) {

      case LOCK_AC_LOCK:
        dout(7) << "handle_lock_dn xlock on " << dname << ", adding (null)" << endl;
        dn = dir->add_dentry(dname);
        break;

      case LOCK_AC_REQXLOCK:
        // send nak
        if (dir->state_test(CDir::STATE_DELETED)) {
          dout(7) << "handle_lock_dn reqxlock on deleted dir " << *dir << ", nak" << endl;
        } else {
          dout(7) << "handle_lock_dn reqxlock on " << dname << " in " << *dir << " dne, nak" << endl;
        }
        {
          MLock *reply = new MLock(LOCK_AC_REQXLOCKNAK, mds->get_nodeid());
          reply->set_dn(dir->dirfrag(), dname);
          reply->set_path(m->get_path());
          mds->send_message_mds(reply, m->get_asker(), MDS_PORT_LOCKER);
        }
         
        // finish request (if we got that far)
	/* FIXME F>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
        if (mdcache->active_requests.count(m)) 
	  mdcache->request_finish(m);
	*/

        delete m;
        return;

      case LOCK_AC_REQXLOCKC:
        dout(7) << "handle_lock_dn reqxlockc on " << dname << " in " << *dir << " dne (yet!)" << endl;
        break;

      default:
        assert(0);
      }
    }
  } else {
    // replica
    if (dir) dn = dir->lookup(dname);
    if (!dn) {
      dout(7) << "handle_lock_dn " << m << " don't have " << m->get_ino() << " dname " << dname << endl;
      
      if (m->get_action() == LOCK_AC_REQXLOCKACK ||
          m->get_action() == LOCK_AC_REQXLOCKNAK) {
        dout(7) << "handle_lock_dn got reqxlockack/nak, but don't have dn " << m->get_path() << ", discovering" << endl;
        //assert(0);  // how can this happen?  tell me now!
        
        vector<CDentry*> trace;
        filepath path = m->get_path();
        int r = mdcache->path_traverse(0, 0, // FIXME FIXME >>>>>>>>>>>>>>>>>>>>>>>>
				       path, trace, true,
				       m, new C_MDS_RetryMessage(mds, m), 
				       MDS_TRAVERSE_DISCOVER);
        assert(r>0);
        return;
      } 

      if (m->get_action() == LOCK_AC_LOCK) {
        if (0) { // not anymore
          dout(7) << "handle_lock_dn don't have " << m->get_path() << ", discovering" << endl;
          
          vector<CDentry*> trace;
          filepath path = m->get_path();
          int r = mdcache->path_traverse(0, 0, // FIXME >>>>>>>>>>>>>>>>>>>>>>>>
					 path, trace, true,
					 m, new C_MDS_RetryMessage(mds,m), 
					 MDS_TRAVERSE_DISCOVER);
          assert(r>0);
        }
        if (1) {
          // NAK
          MLock *reply = new MLock(LOCK_AC_LOCKNAK, mds->get_nodeid());
          reply->set_dn(m->get_dirfrag(), dname);
          mds->send_message_mds(reply, m->get_asker(), MDS_PORT_LOCKER);
        }
      } else {
        dout(7) << "safely ignoring." << endl;
        delete m;
      }
      return;
    }

    assert(dn);
  }

  if (dn) {
    dout(7) << "handle_lock_dn a=" << m->get_action() << " from " << from << " " << *dn << endl;
  } else {
    dout(7) << "handle_lock_dn a=" << m->get_action() << " from " << from << " " << dname << " in " << *dir << endl;
  }
  
  switch (m->get_action()) {
    // -- replica --
  case LOCK_AC_LOCK:
    assert(dn->lockstate == DN_LOCK_SYNC ||
           dn->lockstate == DN_LOCK_UNPINNING ||
           dn->lockstate == DN_LOCK_XLOCK);   // <-- bc the handle_lock_dn did the discover!

    if (dn->is_pinned()) {
      dn->lockstate = DN_LOCK_UNPINNING;

      // wait
      dout(7) << "dn pinned, waiting " << *dn << endl;
      dn->dir->add_waiter(CDir::WAIT_DNUNPINNED,
                          dn->name,
                          new C_MDS_RetryMessage(mds, m));
      return;
    } else {
      dn->lockstate = DN_LOCK_XLOCK;
      dn->xlockedby = 0;

      // ack now
      MLock *reply = new MLock(LOCK_AC_LOCKACK, mds->get_nodeid());
      reply->set_dn(dir->dirfrag(), dname);
      mds->send_message_mds(reply, from, MDS_PORT_LOCKER);
    }

    // wake up waiters
    dir->finish_waiting(CDir::WAIT_DNLOCK, dname);   // ? will this happen on replica ? 
    break;

  case LOCK_AC_SYNC:
    assert(dn->lockstate == DN_LOCK_XLOCK);
    dn->lockstate = DN_LOCK_SYNC;
    dn->xlockedby = 0;

    // null?  hose it.
    if (dn->is_null()) {
      dout(7) << "hosing null (and now sync) dentry " << *dn << endl;
      dir->remove_dentry(dn);
    }

    // wake up waiters
    dir->finish_waiting(CDir::WAIT_DNREAD, dname);   // will this happen either?  YES: if a rename lock backs out
    break;

  case LOCK_AC_REQXLOCKACK:
  case LOCK_AC_REQXLOCKNAK:
    {
      dout(10) << "handle_lock_dn got ack/nak on a reqxlock for " << *dn << endl;
      list<Context*> finished;
      dir->take_waiting(CDir::WAIT_DNREQXLOCK, m->get_dn(), finished, 1);  // TAKE ONE ONLY!
      finish_contexts(finished, 
                      (m->get_action() == LOCK_AC_REQXLOCKACK) ? 1:-1);
    }
    break;


    // -- auth --
  case LOCK_AC_LOCKACK:
  case LOCK_AC_LOCKNAK:
    assert(dn->gather_set.count(from) == 1);
    dn->gather_set.erase(from);
    if (dn->gather_set.size() == 0) {
      dout(7) << "handle_lock_dn finish gather, now xlock on " << *dn << endl;
      dn->lockstate = DN_LOCK_XLOCK;
      mdcache->active_requests[dn->xlockedby->reqid].dentry_xlocks.insert(dn);
      mdcache->active_requests[dn->xlockedby->reqid].dentry_locks.insert(dn);
      dir->finish_waiting(CDir::WAIT_DNLOCK, dname);
    }
    break;


  case LOCK_AC_REQXLOCKC:
    // make sure it's a _file_, if it exists.
    if (dn && dn->inode && dn->inode->is_dir()) {
      dout(7) << "handle_lock_dn failing, reqxlockc on dir " << *dn->inode << endl;
      
      // nak
      string path;
      dn->make_path(path);

      MLock *reply = new MLock(LOCK_AC_REQXLOCKNAK, mds->get_nodeid());
      reply->set_dn(dir->dirfrag(), dname);
      reply->set_path(path);
      mds->send_message_mds(reply, m->get_asker(), MDS_PORT_LOCKER);
      
      assert(0); // FIXME
      /*
      // done
      if (mdcache->active_requests.count(m)) 
        mdcache->request_finish(m);
      else
        delete m;
      */
      return;
    }

    /* REWRITE ME HELP
  case LOCK_AC_REQXLOCK:
    if (dn) {
      dout(7) << "handle_lock_dn reqxlock on " << *dn << endl;
    } else {
      dout(7) << "handle_lock_dn reqxlock on " << dname << " in " << *dir << endl;      
    }
    

    // start request?
    if (!mdcache->active_requests.count(m)) {
      vector<CDentry*> trace;
      if (!mdcache->request_start(m, dir->inode, trace))
        return;  // waiting for pin
    }
    
    // try to xlock!
    if (!dn) {
      assert(m->get_action() == LOCK_AC_REQXLOCKC);
      dn = dir->add_dentry(dname);
    }

    if (dn->xlockedby != m) {
      if (!dentry_xlock_start(dn, m, dir->inode)) {
        // hose null dn if we're waiting on something
        if (dn->is_clean() && dn->is_null() && dn->is_sync()) dir->remove_dentry(dn);
        return;    // waiting for xlock
      }
    } else {
      // successfully xlocked!  on behalf of requestor.
      string path;
      dn->make_path(path);

      dout(7) << "handle_lock_dn reqxlock success for " << m->get_asker() << " on " << *dn << ", acking" << endl;
      
      // ACK xlock request
      MLock *reply = new MLock(LOCK_AC_REQXLOCKACK, mds->get_nodeid());
      reply->set_dn(dir->dirfrag(), dname);
      reply->set_path(path);
      mds->send_message_mds(reply, m->get_asker(), MDS_PORT_LOCKER);

      // note: keep request around in memory (to hold the xlock/pins on behalf of requester)
      return;
    }
    break;
*/

  case LOCK_AC_UNXLOCK:
    dout(7) << "handle_lock_dn unxlock on " << *dn << endl;
    {
      MDRequest *mdr = dn->xlockedby;

      // finish request
      mdcache->request_finish(mdr);  // this will drop the locks (and unpin paths!)
      return;
    }
    break;

  default:
    assert(0);
  }

  delete m;
}







