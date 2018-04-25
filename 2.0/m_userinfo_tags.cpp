/* $ModDesc: Adds the ability for opers to associate certain 'tags' with a user. */
/* $ModAuthor: linuxdaemon */
/* $ModAuthorMail: linuxdaemon@snoonet.org */
/* $ModDepends: core 2.0 */

#include "inspircd.h"

#define EXTBAN_CHAR 't'
#define MAX_TAG_LINE 320

enum
{
	RPL_TAGS = 752,
	RPL_NOTAGS = 753,

	RPL_TAG_WHOIS = 310
};

typedef std::string Tag;
typedef std::set<Tag> TagSet;
typedef std::map<Tag, bool> TagMask;

class UserInfo
{
public:
	UserInfo() : tagStrSize(0)
	{
	}

	bool AddTag(const Tag& tag)
	{
		if ((tagStrSize + tag.length() + 1) > MAX_TAG_LINE)
			return false;

		if (tags.insert(tag).second)
			tagStrSize += tag.length() + 1;

		return true;
	}

	void DelTag(const Tag& tag)
	{
		if (tags.erase(tag))
			tagStrSize -= (tag.length() + 1);
	}

	std::string str() const
	{
		if (this->tags.empty())
			return "";

		std::ostringstream sstr;

		for (TagSet::const_iterator it = tags.begin(), it_end = tags.end(); it != it_end; ++it)
			sstr << ',' << *it;

		return sstr.str().substr(1);
	}

	void fromStr(const std::string& value)
	{
		irc::commasepstream stream(value);
		std::string token;
		while (stream.GetToken(token))
		{
			if (token.empty())
				continue;

			AddTag(token);
		}
	}

	bool MatchTagMask(const TagMask& tm) const
	{
		for (TagMask::const_iterator it = tm.begin(), it_end = tm.end(); it != it_end; ++it)
			if ((tags.count(it->first) != 0) != it->second)
				return false;

		return true;
	}

	bool ApplyTagMask(const TagMask& tm)
	{
		for (TagMask::const_iterator it = tm.begin(), it_end = tm.end(); it != it_end; ++it)
		{
			if (it->second)
			{
				if (!AddTag(it->first))
					return false;
			}
			else
				DelTag(it->first);
		}
		return true;
	}

	void clear()
	{
		tags.clear();
		tagStrSize = 0;
	}

	bool empty() const
	{
		return tags.empty();
	}

private:
	TagSet tags;
	Tag::size_type tagStrSize;
};

class UserInfoExt : public SimpleExtItem<UserInfo>
{
 public:
	UserInfoExt(Module *parent) : SimpleExtItem("user-info", parent)
	{
	}

	std::string serialize(SerializeFormat format, const Extensible* container, void* item) const
	{
		if (!item)
			return "";

		return static_cast<UserInfo*>(item)->str();
	}

	void unserialize(SerializeFormat format, Extensible* container, const std::string& value)
	{
		UserInfo* info = new UserInfo;
		set(container, info);

		info->fromStr(value);
	}

	UserInfo *get_user(User *u)
	{
		UserInfo *info = this->get(u);
		if (!info)
		{
			info = new UserInfo;
			this->set(u, info);
		}

		return info;
	}
};

static TagMask parseTagMask(const std::string& text)
{
	TagMask tm;
	irc::commasepstream stream(text);
	std::string token;
	while (stream.GetToken(token))
	{
		if (token.empty())
			continue;

		bool add;
		switch (token[0])
		{
			case '-':
				token.erase(0, 1);
				add = false;
				break;
			case '+':
				token.erase(0, 1);
			default:
				add = true;
				break;
		}

		tm[token] = add;
	}
	return tm;
}

class UserInfoCommand : public Command
{
 public:
	UserInfoExt ext;
	std::string validTagChars;
	std::string::size_type maxTagLength;

	UserInfoCommand(Module *me) : Command(me, "USERINFO", 1), ext(me)
	{
		syntax = "<target> [{+|-}info]";
		flags_needed = 'o';
		TRANSLATE3(TR_NICK, TR_TEXT, TR_END);
	}

	bool ApplyTagMaskToUser(const std::string& mask, User* user)
	{
		UserInfo* info = ext.get_user(user);
		TagMask tm = parseTagMask(mask);
		for (TagMask::const_iterator it = tm.begin(), it_end = tm.end(); it != it_end; ++it)
		{
			if (!isValidTag(it->first))
				return false;

			if (it->second)
			{
				if (!info->AddTag(it->first))
					return false;
			}
			else
				info->DelTag(it->first);
		}

		std::string seralized = ext.serialize(FORMAT_USER, user, info);

		ServerInstance->PI->SendMetaData(user, ext.name, seralized);
		return true;
	}

	CmdResult Handle(const parameterlist& parameters, User* user)
	{
		std::string target = parameters[0];
		User* target_user = ServerInstance->FindNick(target);
		if (!target_user)
		{
			user->WriteNumeric(ERR_NOSUCHNICK, "%s %s :No such nick", user->nick.c_str(), target.c_str());
			return CMD_FAILURE;
		}

		if (parameters.size() > 1)
		{
			if (!ApplyTagMaskToUser(parameters[1], target_user))
			{
				user->WriteServ("NOTICE %s :Unable to add tags", user->nick.c_str());
				return CMD_FAILURE;
			}
		}

		UserInfo* info = ext.get_user(target_user);
		if (info->empty())
		{
			user->WriteNumeric(RPL_NOTAGS, "%s %s :has no tags", user->nick.c_str(), target_user->nick.c_str());
		}
		else
		{
			std::string seralized = ext.serialize(FORMAT_USER, target_user, info);
			user->WriteNumeric(RPL_TAGS, "%s %s %s :has tags", user->nick.c_str(), target_user->nick.c_str(),
							   seralized.c_str());
		}

		return CMD_SUCCESS;
	}

	bool isValidTag(const std::string &tag)
	{
		if (maxTagLength > 0 && tag.length() > maxTagLength)
			return false;

		if (!validTagChars.empty() && tag.find_first_not_of(validTagChars) != std::string::npos)
			return false;

		return tag.find_first_of(", :") == std::string::npos;
	}
};

class UserInfoModule : public Module
{
	bool onlyOpersSeeTags;
	UserInfoCommand cmd;

	bool MatchInfo(User* u, const std::string& mask)
	{
		UserInfo *info = cmd.ext.get_user(u);
		return info->MatchTagMask(parseTagMask(mask));
	}

 public:
	UserInfoModule() : onlyOpersSeeTags(false), cmd(this)
	{
	}

	void init()
	{
		ServerInstance->Modules->AddService(cmd);
		ServerInstance->Modules->AddService(cmd.ext);
		Implementation eventlist[] = { I_OnRehash, I_OnCheckBan, I_OnWhois, I_OnUserConnect, I_On005Numeric };
		ServerInstance->Modules->Attach(eventlist, this, sizeof(eventlist)/sizeof(Implementation));
		OnRehash(NULL);
	}

	void OnRehash(User*)
	{
		ConfigTag* tag = ServerInstance->Config->ConfValue("userinfo");
		onlyOpersSeeTags = tag->getBool("operonly");
		cmd.validTagChars = tag->getString("tagchars", "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_.");
		cmd.maxTagLength = tag->getInt("taglength", 32);
	}

	ModResult OnCheckBan(User* user, Channel* chan, const std::string& mask)
	{
		if (mask.length() > 2 && mask[0] == EXTBAN_CHAR && mask[1] == ':')
		{
			if (MatchInfo(user, mask.substr(2)))
				return MOD_RES_DENY;
		}
		return MOD_RES_PASSTHRU;
	}

	void OnWhois(User* user, User* dest)
	{
		if (onlyOpersSeeTags && !user->HasPrivPermission("users/auspex"))
			return;

		UserInfo* info = cmd.ext.get_user(dest);
		if (info->empty())
			return;

		std::string serealized = cmd.ext.serialize(FORMAT_USER, dest, info);
		ServerInstance->SendWhoisLine(user, dest, RPL_TAG_WHOIS, "%s %s :has tags: %s", user->nick.c_str(), dest->nick.c_str(), serealized.c_str());
	}

	void OnUserConnect(LocalUser* user)
	{
		std::string tag_mask = user->GetClass()->config->getString("userinfo");
		cmd.ApplyTagMaskToUser(tag_mask, user);
	}

	void On005Numeric(std::string &output)
	{
		ServerInstance->AddExtBanChar(EXTBAN_CHAR);
		output.append(" USERTAGS");
	}

	Version GetVersion()
	{
		return Version("Adds the ability for opers to associate certain 'tags' with a user.");
	}
};

MODULE_INIT(UserInfoModule)
