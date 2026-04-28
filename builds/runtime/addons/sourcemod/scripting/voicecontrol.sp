#pragma semicolon 1

#include <sourcemod>
#include <voicecontrol>

#define PLUGIN_VERSION "0.1.0"
#define TABLE_NAME "voicecontrol_players"
#define STEAM_ID_BUF_SIZE 32

#pragma newdecls required

ConVar g_Cvar_Database;

Database g_Database = null;
char g_Driver[64];
int g_SelectedTargetUserId[MAXPLAYERS + 1];

public Extension __ext_voicecontrol =
{
	name = "VoiceControl",
	file = "voicecontrol.ext",
	required = 1,
};

public Plugin myinfo =
{
	name = "[TF2/OF] Voice Control",
	author = "PGZ",
	description = "Optimized voice gain and auto-level control",
	version = PLUGIN_VERSION,
	url = "https://www.scg.wtf"
};

public void OnPluginStart()
{
	g_Cvar_Database = CreateConVar("vc_database", "default", "Database configuration to use from databases.cfg");

	RegAdminCmd("sm_vc", Command_VoiceControlMenu, ADMFLAG_GENERIC, "Opens Voice Control admin menu");

	SQL_OpenConnection();
}

public void OnClientPostAdminCheck(int client)
{
	if (g_Database == null || !IsValidClient(client))
	{
		return;
	}

	LoadClientSettings(client);
}

public void OnClientDisconnect(int client)
{
	VC_ClearPlayerGain(client);
	VC_ClearPlayerDuckGain(client);
	VC_ClearPlayerPreset(client);
}

void SQL_OpenConnection()
{
	char database[64];
	g_Cvar_Database.GetString(database, sizeof(database));

	if (!SQL_CheckConfig(database))
	{
		SetFailState("Failed to load database config %s from databases.cfg", database);
		return;
	}

	SQL_TConnect(T_InitDatabase, database);
}

public void T_InitDatabase(Handle owner, Database db, const char[] error, any data)
{
	if (db == null)
	{
		SetFailState("DATABASE FAILURE: %s", error);
		return;
	}

	g_Database = db;
	SQL_ReadDriver(g_Database, g_Driver, sizeof(g_Driver));

	if (!StrEqual(g_Driver, "sqlite") && !StrEqual(g_Driver, "mysql"))
	{
		SetFailState("Unsupported database driver %s", g_Driver);
		return;
	}

	char query[512];
	FormatEx(query, sizeof(query),
		"CREATE TABLE IF NOT EXISTS `%s` (steamid64 VARCHAR(32) PRIMARY KEY, gain_db FLOAT NOT NULL DEFAULT 0, enabled TINYINT NOT NULL DEFAULT 0)",
		TABLE_NAME);
	SQL_TQuery(g_Database, T_CreateTable, query);
}

public void T_CreateTable(Database db, DBResultSet results, const char[] error, any data)
{
	if (results == null || error[0] != '\0')
	{
		SetFailState("[VoiceControl] Failed to create table: %s", error);
		return;
	}

	for (int client = 1; client <= MaxClients; client++)
	{
		if (IsValidClient(client))
		{
			LoadClientSettings(client);
		}
	}
}

void LoadClientSettings(int client)
{
	char steamId[STEAM_ID_BUF_SIZE];
	if (!GetClientAuthId(client, AuthId_SteamID64, steamId, sizeof(steamId)))
	{
		VC_ClearPlayerGain(client);
		return;
	}

	char query[256];
	FormatEx(query, sizeof(query), "SELECT gain_db, enabled FROM `%s` WHERE steamid64 = '%s'", TABLE_NAME, steamId);
	SQL_TQuery(g_Database, T_LoadClientSettings, query, GetClientUserId(client));
}

public void T_LoadClientSettings(Database db, DBResultSet results, const char[] error, any userId)
{
	if (results == null || error[0] != '\0')
	{
		LogError("[VoiceControl] Failed to load settings: %s", error);
		return;
	}

	int client = GetClientOfUserId(userId);
	if (!IsValidClient(client))
	{
		return;
	}

	if (!SQL_FetchRow(results))
	{
		VC_ClearPlayerGain(client);
		return;
	}

	float gainDb = SQL_FetchFloat(results, 0);
	bool enabled = SQL_FetchInt(results, 1) != 0;

	if (enabled)
	{
		VC_SetPlayerGain(client, gainDb);
	}
	else
	{
		VC_ClearPlayerGain(client);
	}
}

public Action Command_VoiceControlMenu(int client, int args)
{
	if (!IsValidClient(client))
	{
		return Plugin_Handled;
	}

	ShowPlayerMenu(client);
	return Plugin_Handled;
}

void ShowPlayerMenu(int client)
{
	Menu menu = new Menu(PlayerMenuHandler);
	menu.SetTitle("Voice Control");

	char info[16];
	char name[64];
	for (int target = 1; target <= MaxClients; target++)
	{
		if (!IsValidClient(target) || IsFakeClient(target))
		{
			continue;
		}

		float gainDb = VC_GetPlayerGain(target);
		VoiceControlPreset preset = VC_GetPlayerPreset(target);
		IntToString(GetClientUserId(target), info, sizeof(info));
		char gainLabel[32];
		char presetLabel[32];
		FormatGainDb(gainLabel, sizeof(gainLabel), gainDb);
		FormatPresetName(presetLabel, sizeof(presetLabel), preset);
		FormatEx(name, sizeof(name), "%N (%s, %s)", target, gainLabel, presetLabel);
		menu.AddItem(info, name);
	}

	if (menu.ItemCount == 0)
	{
		PrintToChat(client, "[VoiceControl] No online players.");
		delete menu;
		return;
	}

	menu.ExitButton = true;
	menu.Display(client, 20);
}

public int PlayerMenuHandler(Menu menu, MenuAction action, int client, int param2)
{
	if (action == MenuAction_Select)
	{
		char info[16];
		if (menu.GetItem(param2, info, sizeof(info)))
		{
			int target = GetClientOfUserId(StringToInt(info));
			if (!IsValidClient(target))
			{
				PrintToChat(client, "[VoiceControl] Player is no longer online.");
				return 0;
			}

			g_SelectedTargetUserId[client] = GetClientUserId(target);
			ShowGainMenu(client, target);
		}
	}
	else if (action == MenuAction_End)
	{
		delete menu;
	}

	return 0;
}

void ShowGainMenu(int client, int target)
{
	Menu menu = new Menu(GainMenuHandler);

	char title[96];
	char gainLabel[32];
	char presetLabel[32];
	FormatGainDb(gainLabel, sizeof(gainLabel), VC_GetPlayerGain(target));
	FormatPresetName(presetLabel, sizeof(presetLabel), VC_GetPlayerPreset(target));
	FormatEx(title, sizeof(title), "Voice control for %N\nGain: %s | Preset: %s", target, gainLabel, presetLabel);
	menu.SetTitle(title);

	AddGainItem(menu, -12.0);
	AddGainItem(menu, -9.0);
	AddGainItem(menu, -6.0);
	AddGainItem(menu, -3.0);
	AddGainItem(menu, 0.0);
	AddGainItem(menu, 3.0);
	AddGainItem(menu, 6.0);
	AddGainItem(menu, 9.0);
	AddGainItem(menu, 12.0);
	menu.AddItem("preset_header", "Presets", ITEMDRAW_DISABLED);
	AddPresetItem(menu, VC_Preset_Normal);
	AddPresetItem(menu, VC_Preset_QuietMic);
	AddPresetItem(menu, VC_Preset_NoisyMic);
	AddPresetItem(menu, VC_Preset_LoudMic);

	menu.ExitButton = true;
	menu.ExitBackButton = true;
	menu.Display(client, 20);
}

void AddGainItem(Menu menu, float gainDb)
{
	char info[16];
	char label[32];
	FloatToString(gainDb, info, sizeof(info));

	if (gainDb == 0.0)
	{
		FormatEx(label, sizeof(label), "Reset (0 dB)");
	}
	else
	{
		FormatGainDb(label, sizeof(label), gainDb);
	}

	menu.AddItem(info, label);
}

void AddPresetItem(Menu menu, VoiceControlPreset preset)
{
	char info[8];
	char label[64];
	FormatEx(info, sizeof(info), "p%d", view_as<int>(preset));
	FormatPresetName(label, sizeof(label), preset);
	menu.AddItem(info, label);
}

void FormatGainDb(char[] buffer, int maxlength, float gainDb)
{
	if (gainDb > 0.0)
	{
		FormatEx(buffer, maxlength, "+%.0f dB", gainDb);
	}
	else
	{
		FormatEx(buffer, maxlength, "%.0f dB", gainDb);
	}
}

void FormatPresetName(char[] buffer, int maxlength, VoiceControlPreset preset)
{
	switch (preset)
	{
		case VC_Preset_QuietMic:
		{
			FormatEx(buffer, maxlength, "Quiet mic");
		}
		case VC_Preset_NoisyMic:
		{
			FormatEx(buffer, maxlength, "Noisy mic");
		}
		case VC_Preset_LoudMic:
		{
			FormatEx(buffer, maxlength, "Loud/clipping mic");
		}
		default:
		{
			FormatEx(buffer, maxlength, "Normal");
		}
	}
}

public int GainMenuHandler(Menu menu, MenuAction action, int client, int param2)
{
	if (action == MenuAction_Select)
	{
		int target = GetClientOfUserId(g_SelectedTargetUserId[client]);
		if (!IsValidClient(target))
		{
			PrintToChat(client, "[VoiceControl] Player is no longer online.");
			return 0;
		}

		char info[16];
		if (menu.GetItem(param2, info, sizeof(info)))
		{
			if (info[0] == 'p')
			{
				int presetValue = info[1] - '0';
				ApplyPreset(client, target, view_as<VoiceControlPreset>(presetValue));
			}
			else
			{
				float gainDb = StringToFloat(info);
				ApplyGain(client, target, gainDb);
			}

			if (IsValidClient(client) && IsValidClient(target))
			{
				ShowGainMenu(client, target);
			}
		}
	}
	else if (action == MenuAction_Cancel)
	{
		if (param2 == MenuCancel_ExitBack)
		{
			ShowPlayerMenu(client);
		}
	}
	else if (action == MenuAction_End)
	{
		delete menu;
	}

	return 0;
}

void ApplyGain(int admin, int target, float gainDb)
{
	if (g_Database == null)
	{
		PrintToChat(admin, "[VoiceControl] Database is not ready yet.");
		return;
	}

	if (gainDb == 0.0)
	{
		VC_ClearPlayerGain(target);
		DeleteStoredGain(target);
		PrintToChat(admin, "[VoiceControl] Reset voice gain for %N.", target);
		return;
	}

	if (!VC_SetPlayerGain(target, gainDb))
	{
		PrintToChat(admin, "[VoiceControl] Failed to set voice gain for %N.", target);
		return;
	}

	StoreGain(target, gainDb);
	char gainLabel[32];
	FormatGainDb(gainLabel, sizeof(gainLabel), gainDb);
	PrintToChat(admin, "[VoiceControl] Set %N voice gain to %s.", target, gainLabel);
}

void ApplyPreset(int admin, int target, VoiceControlPreset preset)
{
	char presetLabel[32];
	FormatPresetName(presetLabel, sizeof(presetLabel), preset);

	if (preset == VC_Preset_Normal)
	{
		VC_ClearPlayerPreset(target);
		PrintToChat(admin, "[VoiceControl] Reset voice preset for %N.", target);
		return;
	}

	if (!VC_SetPlayerPreset(target, preset))
	{
		PrintToChat(admin, "[VoiceControl] Failed to set voice preset for %N.", target);
		return;
	}

	PrintToChat(admin, "[VoiceControl] Set %N voice preset to %s.", target, presetLabel);
}

void StoreGain(int target, float gainDb)
{
	char steamId[STEAM_ID_BUF_SIZE];
	if (!GetClientAuthId(target, AuthId_SteamID64, steamId, sizeof(steamId)))
	{
		return;
	}

	char query[512];
	if (StrEqual(g_Driver, "sqlite"))
	{
		FormatEx(query, sizeof(query),
			"INSERT INTO `%s` (steamid64, gain_db, enabled) VALUES ('%s', %.2f, 1) ON CONFLICT(steamid64) DO UPDATE SET gain_db = %.2f, enabled = 1",
			TABLE_NAME, steamId, gainDb, gainDb);
	}
	else
	{
		FormatEx(query, sizeof(query),
			"INSERT INTO `%s` (steamid64, gain_db, enabled) VALUES ('%s', %.2f, 1) ON DUPLICATE KEY UPDATE gain_db = %.2f, enabled = 1",
			TABLE_NAME, steamId, gainDb, gainDb);
	}

	SQL_TQuery(g_Database, T_ErrorOnly, query);
}

void DeleteStoredGain(int target)
{
	char steamId[STEAM_ID_BUF_SIZE];
	if (!GetClientAuthId(target, AuthId_SteamID64, steamId, sizeof(steamId)))
	{
		return;
	}

	char query[256];
	FormatEx(query, sizeof(query), "DELETE FROM `%s` WHERE steamid64 = '%s'", TABLE_NAME, steamId);
	SQL_TQuery(g_Database, T_ErrorOnly, query);
}

public void T_ErrorOnly(Database db, DBResultSet results, const char[] error, any data)
{
	if (results == null || error[0] != '\0')
	{
		LogError("[VoiceControl] SQL error: %s", error);
	}
}

bool IsValidClient(int client)
{
	return client >= 1 && client <= MaxClients && IsClientInGame(client) && !IsClientReplay(client);
}
