#!/usr/bin/python3 -I
# Adds `RHVoiceVoice "<language>" "<voice>"` to the Speech Dispatcher module of
# RHVoice: the voice the module speaks a language with when the client does
# not ask for a voice by name.  Usage: apply-voice-patch.py <src/sd_module dir>
import sys, pathlib

d = pathlib.Path(sys.argv[1])


def rep(name, old, new):
    p = d / name
    s = p.read_text()
    assert s.count(old) == 1, (name, old[:50])
    p.write_text(s.replace(old, new))


rep('main.cpp', '#include "module.hpp"\n',
    '#include "module.hpp"\n#include "config_tts.hpp"\n')
rep('main.cpp', '  RHVoice::sd::module mod;\n',
    '  // Speech Dispatcher passes the configuration file of the module\n'
    '  if(argc>1)\n'
    '    RHVoice::sd::cmd::read_module_config(argv[1]);\n'
    '  RHVoice::sd::module mod;\n')

rep('config_tts.hpp', '''        void set_voice();
      };
''', '''        void set_voice();
      };

      // Reads the RHVoiceVoice "<language>" "<voice>" lines of the
      // configuration file of the module
      void read_module_config(const char* path);
''')

rep('config_tts.cpp', '''#include <exception>
#include <set>
''', '''#include <cctype>
#include <exception>
#include <fstream>
#include <map>
#include <set>
#include <string>
''')
rep('config_tts.cpp', '''namespace RHVoice
{
  namespace sd
  {
    namespace cmd
    {
      config_tts::config_tts()''', '''namespace
{
  // RHVoiceVoice: the voice for a language, by its two-letter code
  std::map<std::string,std::string> language_voices;

  std::string base_language(const std::string& code)
  {
    std::string result=code.substr(0,code.find_first_of("-_"));
    for(std::string::size_type i=0;i<result.size();++i)
      result[i]=std::tolower(static_cast<unsigned char>(result[i]));
    return result;
  }

  // The next word of a configuration line, or the next "quoted value"
  bool next_value(const std::string& line,std::string::size_type& pos,std::string& value)
  {
    while((pos<line.size())&&std::isspace(static_cast<unsigned char>(line[pos])))
      ++pos;
    if((pos>=line.size())||(line[pos]=='#'))
      return false;
    std::string::size_type end;
    if(line[pos]=='"')
      {
        end=line.find('"',pos+1);
        if(end==std::string::npos)
          return false;
        value=line.substr(pos+1,end-pos-1);
        pos=end+1;
      }
    else
      {
        end=pos;
        while((end<line.size())&&!std::isspace(static_cast<unsigned char>(line[end])))
          ++end;
        value=line.substr(pos,end-pos);
        pos=end;
      }
    return true;
  }
}

namespace RHVoice
{
  namespace sd
  {
    namespace cmd
    {
      void read_module_config(const char* path)
      {
        std::ifstream file(path);
        std::string line,key,language,voice;
        while(std::getline(file,line))
          {
            std::string::size_type pos=0;
            if(!next_value(line,pos,key)||(base_language(key)!="rhvoicevoice"))
              continue;
            if(next_value(line,pos,language)&&next_value(line,pos,voice)&&!voice.empty())
              language_voices[base_language(language)]=voice;
          }
      }

      config_tts::config_tts()''')
rep('config_tts.cpp', '''                else
                  new_voice=voice1;
              }
''', '''                else
                  new_voice=voice1;
                // the voice configured for the language wins over the voice type
                std::map<std::string,std::string>::const_iterator configured=
                  language_voices.find(base_language(tts_settings.language_code.get()));
                if(configured!=language_voices.end())
                  for(std::set<voice_profile>::const_iterator it=profiles.begin();it!=profiles.end();++it)
                    if(it->get_name()==configured->second)
                      {
                        new_voice=*it;
                        break;
                      }
              }
''')
print('patched', d)
