#include <config.h>

#include <apt-pkg/acquire-method.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/error.h>
#include <apt-pkg/gpgv.h>
#include <apt-pkg/strutl.h>
#include <apt-pkg/fileutl.h>
#include "aptmethod.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <sstream>
#include <iterator>
#include <iostream>
#include <string>
#include <vector>

#include <apti18n.h>

using std::string;
using std::vector;

#define GNUPGPREFIX "[GNUPG:]"
#define GNUPGBADSIG "[GNUPG:] BADSIG"
#define GNUPGNOPUBKEY "[GNUPG:] NO_PUBKEY"
#define GNUPGVALIDSIG "[GNUPG:] VALIDSIG"
#define GNUPGGOODSIG "[GNUPG:] GOODSIG"
#define GNUPGKEYEXPIRED "[GNUPG:] KEYEXPIRED"
#define GNUPGREVKEYSIG "[GNUPG:] REVKEYSIG"
#define GNUPGNODATA "[GNUPG:] NODATA"

struct Digest {
   enum class State {
      Untrusted,
      Weak,
      Trusted,
   } state;
   char name[32];
};

static constexpr Digest Digests[] = {
   {Digest::State::Untrusted, "Invalid digest"},
   {Digest::State::Untrusted, "MD5"},
   {Digest::State::Weak, "SHA1"},
   {Digest::State::Weak, "RIPE-MD/160"},
   {Digest::State::Trusted, "Reserved digest"},
   {Digest::State::Trusted, "Reserved digest"},
   {Digest::State::Trusted, "Reserved digest"},
   {Digest::State::Trusted, "Reserved digest"},
   {Digest::State::Trusted, "SHA256"},
   {Digest::State::Trusted, "SHA384"},
   {Digest::State::Trusted, "SHA512"},
   {Digest::State::Trusted, "SHA224"},
};

static Digest FindDigest(std::string const & Digest)
{
   int id = atoi(Digest.c_str());
   if (id >= 0 && static_cast<unsigned>(id) < _count(Digests)) {
      return Digests[id];
   } else {
      return Digests[0];
   }
}

struct Signer {
   std::string key;
   std::string note;
};

class GPGVMethod : public aptMethod
{
   private:
   string VerifyGetSigners(const char *file, const char *outfile,
				std::string const &key,
				vector<string> &GoodSigners,
                                vector<string> &BadSigners,
                                vector<string> &WorthlessSigners,
                                vector<Signer> &SoonWorthlessSigners,
				vector<string> &NoPubKeySigners);
   protected:
   virtual bool URIAcquire(std::string const &Message, FetchItem *Itm) APT_OVERRIDE;
   public:
   
   GPGVMethod() : aptMethod("gpgv","1.0",SingleInstance | SendConfig) {};
};

string GPGVMethod::VerifyGetSigners(const char *file, const char *outfile,
					 std::string const &key,
					 vector<string> &GoodSigners,
					 vector<string> &BadSigners,
					 vector<string> &WorthlessSigners,
					 vector<Signer> &SoonWorthlessSigners,
					 vector<string> &NoPubKeySigners)
{
   bool const Debug = _config->FindB("Debug::Acquire::gpgv", false);

   if (Debug == true)
      std::clog << "inside VerifyGetSigners" << std::endl;

   int fd[2];
   bool const keyIsID = (key.empty() == false && key[0] != '/');

   if (pipe(fd) < 0)
      return "Couldn't create pipe";

   pid_t pid = fork();
   if (pid < 0)
      return string("Couldn't spawn new process") + strerror(errno);
   else if (pid == 0)
      ExecGPGV(outfile, file, 3, fd, (keyIsID ? "" : key));
   close(fd[1]);

   FILE *pipein = fdopen(fd[0], "r");

   // Loop over the output of apt-key (which really is gnupg), and check the signatures.
   std::vector<std::string> ValidSigners;
   size_t buffersize = 0;
   char *buffer = NULL;
   while (1)
   {
      if (getline(&buffer, &buffersize, pipein) == -1)
	 break;
      if (Debug == true)
         std::clog << "Read: " << buffer << std::endl;

      // Push the data into three separate vectors, which
      // we later concatenate.  They're kept separate so
      // if we improve the apt method communication stuff later
      // it will be better.
      if (strncmp(buffer, GNUPGBADSIG, sizeof(GNUPGBADSIG)-1) == 0)
      {
         if (Debug == true)
            std::clog << "Got BADSIG! " << std::endl;
         BadSigners.push_back(string(buffer+sizeof(GNUPGPREFIX)));
      }
      else if (strncmp(buffer, GNUPGNOPUBKEY, sizeof(GNUPGNOPUBKEY)-1) == 0)
      {
         if (Debug == true)
            std::clog << "Got NO_PUBKEY " << std::endl;
         NoPubKeySigners.push_back(string(buffer+sizeof(GNUPGPREFIX)));
      }
      else if (strncmp(buffer, GNUPGNODATA, sizeof(GNUPGBADSIG)-1) == 0)
      {
         if (Debug == true)
            std::clog << "Got NODATA! " << std::endl;
         BadSigners.push_back(string(buffer+sizeof(GNUPGPREFIX)));
      }
      else if (strncmp(buffer, GNUPGKEYEXPIRED, sizeof(GNUPGKEYEXPIRED)-1) == 0)
      {
         if (Debug == true)
            std::clog << "Got KEYEXPIRED! " << std::endl;
         WorthlessSigners.push_back(string(buffer+sizeof(GNUPGPREFIX)));
      }
      else if (strncmp(buffer, GNUPGREVKEYSIG, sizeof(GNUPGREVKEYSIG)-1) == 0)
      {
         if (Debug == true)
            std::clog << "Got REVKEYSIG! " << std::endl;
         WorthlessSigners.push_back(string(buffer+sizeof(GNUPGPREFIX)));
      }
      else if (strncmp(buffer, GNUPGGOODSIG, sizeof(GNUPGGOODSIG)-1) == 0)
      {
         char *sig = buffer + sizeof(GNUPGPREFIX);
         char *p = sig + sizeof("GOODSIG");
         while (*p && isxdigit(*p)) 
            p++;
         *p = 0;
         if (Debug == true)
            std::clog << "Got GOODSIG, key ID:" << sig << std::endl;
         GoodSigners.push_back(string(sig));
      }
      else if (strncmp(buffer, GNUPGVALIDSIG, sizeof(GNUPGVALIDSIG)-1) == 0)
      {
         char *sig = buffer + sizeof(GNUPGVALIDSIG);
         std::istringstream iss((string(sig)));
         vector<string> tokens{std::istream_iterator<string>{iss},
                               std::istream_iterator<string>{}};
         char *p = sig;
         while (*p && isxdigit(*p))
            p++;
         *p = 0;
         if (Debug == true)
            std::clog << "Got VALIDSIG, key ID: " << sig << std::endl;
         // Reject weak digest algorithms
         Digest digest = FindDigest(tokens[7]);
         switch (digest.state) {
         case Digest::State::Weak:
            // Treat them like an expired key: For that a message about expiry
            // is emitted, a VALIDSIG, but no GOODSIG.
            SoonWorthlessSigners.push_back({string(sig), digest.name});
            break;
         case Digest::State::Untrusted:
            // Treat them like an expired key: For that a message about expiry
            // is emitted, a VALIDSIG, but no GOODSIG.
            WorthlessSigners.push_back(string(sig));
            GoodSigners.erase(std::remove(GoodSigners.begin(), GoodSigners.end(), string(sig)));
            break;
         case Digest::State::Trusted:
            break;
         }

         ValidSigners.push_back(string(sig));
      }
   }
   fclose(pipein);
   free(buffer);

   // apt-key has a --keyid parameter, but this requires gpg, so we call it without it
   // and instead check after the fact which keyids where used for verification
   if (keyIsID == true)
   {
      if (Debug == true)
	 std::clog << "GoodSigs needs to be limited to keyid " << key << std::endl;
      std::vector<std::string>::iterator const foundItr = std::find(ValidSigners.begin(), ValidSigners.end(), key);
      bool const found = (foundItr != ValidSigners.end());
      std::copy(GoodSigners.begin(), GoodSigners.end(), std::back_insert_iterator<std::vector<std::string> >(NoPubKeySigners));
      if (found)
      {
	 // we look for GOODSIG here as well as an expired sig is a valid sig as well (but not a good one)
	 std::string const goodlongkeyid = "GOODSIG " + key.substr(24, 16);
	 bool const foundGood = std::find(GoodSigners.begin(), GoodSigners.end(), goodlongkeyid) != GoodSigners.end();
	 if (Debug == true)
	    std::clog << "Key " << key << " is valid sig, is " << goodlongkeyid << " also a good one? " << (foundGood ? "yes" : "no") << std::endl;
	 GoodSigners.clear();
	 if (foundGood)
	 {
	    GoodSigners.push_back(goodlongkeyid);
	    NoPubKeySigners.erase(std::remove(NoPubKeySigners.begin(), NoPubKeySigners.end(), goodlongkeyid), NoPubKeySigners.end());
	 }
      }
      else
	 GoodSigners.clear();
   }

   int status;
   waitpid(pid, &status, 0);
   if (Debug == true)
   {
      ioprintf(std::clog, "gpgv exited with status %i\n", WEXITSTATUS(status));
   }
   
   if (WEXITSTATUS(status) == 0)
   {
      if (keyIsID)
      {
	 // gpgv will report success, but we want to enforce a certain keyring
	 // so if we haven't found the key the valid we found is in fact invalid
	 if (GoodSigners.empty())
	    return _("At least one invalid signature was encountered.");
      }
      else
      {
	 if (GoodSigners.empty())
	    return _("Internal error: Good signature, but could not determine key fingerprint?!");
      }
      return "";
   }
   else if (WEXITSTATUS(status) == 1)
      return _("At least one invalid signature was encountered.");
   else if (WEXITSTATUS(status) == 111)
      return _("Could not execute 'apt-key' to verify signature (is gnupg installed?)");
   else if (WEXITSTATUS(status) == 112)
   {
      // acquire system checks for "NODATA" to generate GPG errors (the others are only warnings)
      std::string errmsg;
      //TRANSLATORS: %s is a single techy word like 'NODATA'
      strprintf(errmsg, _("Clearsigned file isn't valid, got '%s' (does the network require authentication?)"), "NODATA");
      return errmsg;
   }
   else
      return _("Unknown error executing apt-key");
}

bool GPGVMethod::URIAcquire(std::string const &Message, FetchItem *Itm)
{
   URI const Get = Itm->Uri;
   string const Path = Get.Host + Get.Path; // To account for relative paths
   std::string const key = LookupTag(Message, "Signed-By");
   vector<string> GoodSigners;
   vector<string> BadSigners;
   // a worthless signature is a expired or revoked one
   vector<string> WorthlessSigners;
   vector<Signer> SoonWorthlessSigners;
   vector<string> NoPubKeySigners;
   
   FetchResult Res;
   Res.Filename = Itm->DestFile;
   URIStart(Res);

   // Run apt-key on file, extract contents and get the key ID of the signer
   string msg = VerifyGetSigners(Path.c_str(), Itm->DestFile.c_str(), key,
                                 GoodSigners, BadSigners, WorthlessSigners,
                                 SoonWorthlessSigners, NoPubKeySigners);


   // Check if there are any good signers that are not soon worthless
   std::vector<std::string> NotWarnAboutSigners(GoodSigners);
   for (auto const & Signer : SoonWorthlessSigners)
      NotWarnAboutSigners.erase(std::remove(NotWarnAboutSigners.begin(), NotWarnAboutSigners.end(), "GOODSIG " + Signer.key));
   // If all signers are soon worthless, report them.
   if (NotWarnAboutSigners.empty()) {
      for (auto const & Signer : SoonWorthlessSigners)
         // TRANSLATORS: The second %s is the reason and is untranslated for repository owners.
         Warning(_("Signature by key %s uses weak digest algorithm (%s)"), Signer.key.c_str(), Signer.note.c_str());
   }

   if (GoodSigners.empty() || !BadSigners.empty() || !NoPubKeySigners.empty())
   {
      string errmsg;
      // In this case, something bad probably happened, so we just go
      // with what the other method gave us for an error message.
      if (BadSigners.empty() && WorthlessSigners.empty() && NoPubKeySigners.empty())
         errmsg = msg;
      else
      {
         if (!BadSigners.empty())
         {
            errmsg += _("The following signatures were invalid:\n");
            for (vector<string>::iterator I = BadSigners.begin();
		 I != BadSigners.end(); ++I)
               errmsg += (*I + "\n");
         }
         if (!WorthlessSigners.empty())
         {
            errmsg += _("The following signatures were invalid:\n");
            for (vector<string>::iterator I = WorthlessSigners.begin();
		 I != WorthlessSigners.end(); ++I)
               errmsg += (*I + "\n");
         }
         if (!NoPubKeySigners.empty())
         {
             errmsg += _("The following signatures couldn't be verified because the public key is not available:\n");
            for (vector<string>::iterator I = NoPubKeySigners.begin();
		 I != NoPubKeySigners.end(); ++I)
               errmsg += (*I + "\n");
         }
      }
      // this is only fatal if we have no good sigs or if we have at
      // least one bad signature. good signatures and NoPubKey signatures
      // happen easily when a file is signed with multiple signatures
      if(GoodSigners.empty() or !BadSigners.empty())
      	 return _error->Error("%s", errmsg.c_str());
   }
      
   // Just pass the raw output up, because passing it as a real data
   // structure is too difficult with the method stuff.  We keep it
   // as three separate vectors for future extensibility.
   Res.GPGVOutput = GoodSigners;
   Res.GPGVOutput.insert(Res.GPGVOutput.end(),BadSigners.begin(),BadSigners.end());
   Res.GPGVOutput.insert(Res.GPGVOutput.end(),NoPubKeySigners.begin(),NoPubKeySigners.end());
   URIDone(Res);

   if (_config->FindB("Debug::Acquire::gpgv", false))
   {
      std::clog << "apt-key succeeded\n";
   }

   return true;
}


int main()
{
   setlocale(LC_ALL, "");

   GPGVMethod Mth;

   return Mth.Run();
}
