/**
 * This file is part of Hercules.
 * http://herc.ws - http://github.com/HerculesWS/Hercules
 *
 * Copyright (C) 2012-2018  Hercules Dev Team
 * Copyright (C)  Athena Dev Teams
 *
 * Hercules is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define HERCULES_CORE

#include "mapif.h"

#include "char/char.h"
#include "char/int_auction.h"
#include "char/int_clan.h"
#include "char/int_guild.h"
#include "char/int_homun.h"
#include "char/int_mail.h"
#include "char/int_rodex.h"
#include "char/inter.h"
#include "common/cbasetypes.h"
#include "common/mmo.h"
#include "common/nullpo.h"
#include "common/random.h"
#include "common/showmsg.h"
#include "common/socket.h"
#include "common/sql.h"
#include "common/strlib.h"

#include <stdlib.h>

void mapif_ban(int id, unsigned int flag, int status)
{
	// send to all map-servers to disconnect the player
	unsigned char buf[11];
	WBUFW(buf,0) = 0x2b14;
	WBUFL(buf,2) = id;
	WBUFB(buf,6) = flag; // 0: change of status, 1: ban
	WBUFL(buf,7) = status; // status or final date of a banishment
	mapif->sendall(buf, 11);
}

/// Initializes a server structure.
void mapif_server_init(int id)
{
	//memset(&chr->server[id], 0, sizeof(server[id]));
	chr->server[id].fd = -1;
}

/// Destroys a server structure.
void mapif_server_destroy(int id)
{
	if (chr->server[id].fd == -1) {
		sockt->close(chr->server[id].fd);
		chr->server[id].fd = -1;
	}
}

/// Resets all the data related to a server.
void mapif_server_reset(int id)
{
	int i, j;
	unsigned char buf[16384];
	int fd = chr->server[id].fd;
	//Notify other map servers that this one is gone. [Skotlex]
	WBUFW(buf, 0) = 0x2b20;
	WBUFL(buf, 4) = htonl(chr->server[id].ip);
	WBUFW(buf, 8) = htons(chr->server[id].port);
	j = 0;
	for (i = 0; i < VECTOR_LENGTH(chr->server[id].maps); i++) {
		uint16 m = VECTOR_INDEX(chr->server[id].maps, i);
		if (m != 0)
			WBUFW(buf, 10 + (j++) * 4) = m;
	}
	if (j > 0) {
		WBUFW(buf, 2) = j * 4 + 10;
		mapif->sendallwos(fd, buf, WBUFW(buf, 2));
	}
	if (SQL_ERROR == SQL->Query(inter->sql_handle, "DELETE FROM `%s` WHERE `index`='%d'", ragsrvinfo_db, chr->server[id].fd))
		Sql_ShowDebug(inter->sql_handle);
	chr->online_char_db->foreach(chr->online_char_db, chr->db_setoffline, id); //Tag relevant chars as 'in disconnected' server.
	mapif->server_destroy(id);
	mapif->server_init(id);
}

/// Called when the connection to a Map Server is disconnected.
void mapif_on_disconnect(int id)
{
	ShowStatus("Map-server #%d has disconnected.\n", id);
	mapif->server_reset(id);
}

void mapif_on_parse_accinfo(int account_id, int u_fd, int u_aid, int u_group, int map_fd)
{
	Assert_retv(chr->login_fd > 0);
	WFIFOHEAD(chr->login_fd, 22);
	WFIFOW(chr->login_fd, 0) = 0x2740;
	WFIFOL(chr->login_fd, 2) = account_id;
	WFIFOL(chr->login_fd, 6) = u_fd;
	WFIFOL(chr->login_fd, 10) = u_aid;
	WFIFOL(chr->login_fd, 14) = u_group;
	WFIFOL(chr->login_fd, 18) = map_fd;
	WFIFOSET(chr->login_fd, 22);
}

void mapif_char_ban(int char_id, time_t timestamp)
{
	unsigned char buf[11];
	WBUFW(buf, 0) = 0x2b14;
	WBUFL(buf, 2) = char_id;
	WBUFB(buf, 6) = 2;
	WBUFL(buf, 7) = (unsigned int)timestamp;
	mapif->sendall(buf, 11);
}

int mapif_sendall(const unsigned char *buf, unsigned int len)
{
	int i, c;

	nullpo_ret(buf);
	c = 0;
	for (i = 0; i < ARRAYLENGTH(chr->server); i++) {
		int fd;
		if ((fd = chr->server[i].fd) > 0) {
			WFIFOHEAD(fd, len);
			memcpy(WFIFOP(fd, 0), buf, len);
			WFIFOSET(fd, len);
			c++;
		}
	}

	return c;
}

int mapif_sendallwos(int sfd, unsigned char *buf, unsigned int len)
{
	int i, c;

	nullpo_ret(buf);
	c = 0;
	for (i = 0; i < ARRAYLENGTH(chr->server); i++) {
		int fd;
		if ((fd = chr->server[i].fd) > 0 && fd != sfd) {
			WFIFOHEAD(fd, len);
			memcpy(WFIFOP(fd, 0), buf, len);
			WFIFOSET(fd, len);
			c++;
		}
	}

	return c;
}


int mapif_send(int fd, unsigned char *buf, unsigned int len)
{
	nullpo_ret(buf);
	if (fd >= 0) {
		int i;
		ARR_FIND (0, ARRAYLENGTH(chr->server), i, fd == chr->server[i].fd);
		if (i < ARRAYLENGTH(chr->server)) {
			WFIFOHEAD(fd, len);
			memcpy(WFIFOP(fd, 0), buf, len);
			WFIFOSET(fd, len);
			return 1;
		}
	}
	return 0;
}

void mapif_send_users_count(int users)
{
	uint8 buf[6];
	// send number of players to all map-servers
	WBUFW(buf, 0) = 0x2b00;
	WBUFL(buf, 2) = users;
	mapif->sendall(buf, 6);
}


void mapif_auction_message(int char_id, unsigned char result)
{
	unsigned char buf[74];

	WBUFW(buf, 0) = 0x3854;
	WBUFL(buf, 2) = char_id;
	WBUFL(buf, 6) = result;
	mapif->sendall(buf, 7);
}

void mapif_auction_sendlist(int fd, int char_id, short count, short pages, unsigned char *buf)
{
	int len = (sizeof(struct auction_data) * count) + 12;

	nullpo_retv(buf);

	WFIFOHEAD(fd, len);
	WFIFOW(fd, 0) = 0x3850;
	WFIFOW(fd, 2) = len;
	WFIFOL(fd, 4) = char_id;
	WFIFOW(fd, 8) = count;
	WFIFOW(fd, 10) = pages;
	memcpy(WFIFOP(fd, 12), buf, len - 12);
	WFIFOSET(fd, len);
}

void mapif_parse_auction_requestlist(int fd)
{
	char searchtext[NAME_LENGTH];
	int char_id = RFIFOL(fd, 4), len = sizeof(struct auction_data);
	int price = RFIFOL(fd, 10);
	short type = RFIFOW(fd, 8), page = max(1, RFIFOW(fd, 14));
	unsigned char buf[5 * sizeof(struct auction_data)];
	struct DBIterator *iter = db_iterator(inter_auction->db);
	struct auction_data *auction;
	short i = 0, j = 0, pages = 1;

	memcpy(searchtext, RFIFOP(fd, 16), NAME_LENGTH);

	for (auction = dbi_first(iter); dbi_exists(iter); auction = dbi_next(iter)) {
		if ((type == 0 && auction->type != IT_ARMOR && auction->type != IT_PETARMOR)
		 || (type == 1 && auction->type != IT_WEAPON)
		 || (type == 2 && auction->type != IT_CARD)
		 || (type == 3 && auction->type != IT_ETC)
		 || (type == 4 && !strstr(auction->item_name, searchtext))
		 || (type == 5 && auction->price > price)
		 || (type == 6 && auction->seller_id != char_id)
		 || (type == 7 && auction->buyer_id != char_id))
			continue;

		i++;
		if (i > 5) {
			// Counting Pages of Total Results (5 Results per Page)
			pages++;
			i = 1; // First Result of This Page
		}

		if (page != pages)
			continue; // This is not the requested Page

		memcpy(WBUFP(buf, j * len), auction, len);
		j++; // Found Results
	}
	dbi_destroy(iter);

	mapif->auction_sendlist(fd, char_id, j, pages, buf);
}

void mapif_auction_register(int fd, struct auction_data *auction)
{
	int len = sizeof(struct auction_data) + 4;

	nullpo_retv(auction);

	WFIFOHEAD(fd,len);
	WFIFOW(fd, 0) = 0x3851;
	WFIFOW(fd, 2) = len;
	memcpy(WFIFOP(fd, 4), auction, sizeof(struct auction_data));
	WFIFOSET(fd, len);
}

void mapif_parse_auction_register(int fd)
{
	struct auction_data auction;
	if( RFIFOW(fd, 2) != sizeof(struct auction_data) + 4 )
		return;

	memcpy(&auction, RFIFOP(fd, 4), sizeof(struct auction_data));
	if( inter_auction->count(auction.seller_id, false) < 5 )
		auction.auction_id = inter_auction->create(&auction);

	mapif->auction_register(fd, &auction);
}

void mapif_auction_cancel(int fd, int char_id, unsigned char result)
{
	WFIFOHEAD(fd, 7);
	WFIFOW(fd, 0) = 0x3852;
	WFIFOL(fd, 2) = char_id;
	WFIFOB(fd, 6) = result;
	WFIFOSET(fd, 7);
}

void mapif_parse_auction_cancel(int fd)
{
	int char_id = RFIFOL(fd, 2), auction_id = RFIFOL(fd, 6);
	struct auction_data *auction;

	if ((auction = (struct auction_data *)idb_get(inter_auction->db, auction_id)) == NULL) {
		mapif->auction_cancel(fd, char_id, 1); // Bid Number is Incorrect
		return;
	}

	if (auction->seller_id != char_id) {
		mapif->auction_cancel(fd, char_id, 2); // You cannot end the auction
		return;
	}

	if (auction->buyer_id > 0) {
		mapif->auction_cancel(fd, char_id, 3); // An auction with at least one bidder cannot be canceled
		return;
	}

	inter_mail->sendmail(0, "Auction Manager", auction->seller_id, auction->seller_name, "Auction", "Auction canceled.", 0, &auction->item);
	inter_auction->delete_(auction);

	mapif->auction_cancel(fd, char_id, 0); // The auction has been canceled
}


void mapif_auction_close(int fd, int char_id, unsigned char result)
{
	WFIFOHEAD(fd, 7);
	WFIFOW(fd, 0) = 0x3853;
	WFIFOL(fd, 2) = char_id;
	WFIFOB(fd, 6) = result;
	WFIFOSET(fd, 7);
}

void mapif_parse_auction_close(int fd)
{
	int char_id = RFIFOL(fd, 2), auction_id = RFIFOL(fd, 6);
	struct auction_data *auction;

	if ((auction = (struct auction_data *)idb_get(inter_auction->db, auction_id)) == NULL) {
		mapif->auction_close(fd, char_id, 2); // Bid Number is Incorrect
		return;
	}

	if (auction->seller_id != char_id) {
		mapif->auction_close(fd, char_id, 1); // You cannot end the auction
		return;
	}

	if (auction->buyer_id == 0) {
		mapif->auction_close(fd, char_id, 1); // You cannot end the auction
		return;
	}

	// Send Money to Seller
	inter_mail->sendmail(0, "Auction Manager", auction->seller_id, auction->seller_name, "Auction", "Auction closed.", auction->price, NULL);
	// Send Item to Buyer
	inter_mail->sendmail(0, "Auction Manager", auction->buyer_id, auction->buyer_name, "Auction", "Auction winner.", 0, &auction->item);
	mapif->auction_message(auction->buyer_id, 6); // You have won the auction
	inter_auction->delete_(auction);

	mapif->auction_close(fd, char_id, 0); // You have ended the auction
}

void mapif_auction_bid(int fd, int char_id, int bid, unsigned char result)
{
	WFIFOHEAD(fd, 11);
	WFIFOW(fd, 0) = 0x3855;
	WFIFOL(fd, 2) = char_id;
	WFIFOL(fd, 6) = bid; // To Return Zeny
	WFIFOB(fd, 10) = result;
	WFIFOSET(fd, 11);
}

void mapif_parse_auction_bid(int fd)
{
	int char_id = RFIFOL(fd, 4), bid = RFIFOL(fd, 12);
	unsigned int auction_id = RFIFOL(fd, 8);
	struct auction_data *auction;

	if ((auction = (struct auction_data *)idb_get(inter_auction->db, auction_id)) == NULL || auction->price >= bid || auction->seller_id == char_id) {
		mapif->auction_bid(fd, char_id, bid, 0); // You have failed to bid in the auction
		return;
	}

	if (inter_auction->count(char_id, true) > 4 && bid < auction->buynow && auction->buyer_id != char_id) {
		mapif->auction_bid(fd, char_id, bid, 9); // You cannot place more than 5 bids at a time
		return;
	}

	if (auction->buyer_id > 0) {
		// Send Money back to the previous Buyer
		if (auction->buyer_id != char_id) {
			inter_mail->sendmail(0, "Auction Manager", auction->buyer_id, auction->buyer_name, "Auction", "Someone has placed a higher bid.", auction->price, NULL);
			mapif->auction_message(auction->buyer_id, 7); // You have failed to win the auction
		} else {
			inter_mail->sendmail(0, "Auction Manager", auction->buyer_id, auction->buyer_name, "Auction", "You have placed a higher bid.", auction->price, NULL);
		}
	}

	auction->buyer_id = char_id;
	safestrncpy(auction->buyer_name, RFIFOP(fd, 16), NAME_LENGTH);
	auction->price = bid;

	if (bid >= auction->buynow) {
		// Automatic won the auction
		mapif->auction_bid(fd, char_id, bid - auction->buynow, 1); // You have successfully bid in the auction

		inter_mail->sendmail(0, "Auction Manager", auction->buyer_id, auction->buyer_name, "Auction", "You have won the auction.", 0, &auction->item);
		mapif->auction_message(char_id, 6); // You have won the auction
		inter_mail->sendmail(0, "Auction Manager", auction->seller_id, auction->seller_name, "Auction", "Payment for your auction!.", auction->buynow, NULL);

		inter_auction->delete_(auction);
		return;
	}

	inter_auction->save(auction);

	mapif->auction_bid(fd, char_id, 0, 1); // You have successfully bid in the auction
}

bool mapif_elemental_create(struct s_elemental *ele);
bool mapif_elemental_save(const struct s_elemental *ele);
bool mapif_elemental_load(int ele_id, int char_id, struct s_elemental *ele);
bool mapif_elemental_delete(int ele_id);
void mapif_elemental_send(int fd, struct s_elemental *ele, unsigned char flag);
void mapif_parse_elemental_create(int fd, const struct s_elemental *ele);
void mapif_parse_elemental_load(int fd, int ele_id, int char_id);
void mapif_elemental_deleted(int fd, unsigned char flag);
void mapif_parse_elemental_delete(int fd, int ele_id);
void mapif_elemental_saved(int fd, unsigned char flag);
void mapif_parse_elemental_save(int fd, const struct s_elemental *ele);
int mapif_guild_created(int fd, int account_id, struct guild *g);
int mapif_guild_noinfo(int fd, int guild_id);
int mapif_guild_info(int fd, struct guild *g);
int mapif_guild_memberadded(int fd, int guild_id, int account_id, int char_id, int flag);
int mapif_guild_withdraw(int guild_id, int account_id, int char_id, int flag, const char *name, const char *mes);
int mapif_guild_memberinfoshort(struct guild *g, int idx);
int mapif_guild_broken(int guild_id, int flag);
int mapif_guild_message(int guild_id, int account_id, const char *mes, int len, int sfd);
int mapif_guild_basicinfochanged(int guild_id, int type, const void *data, int len);
int mapif_guild_memberinfochanged(int guild_id, int account_id, int char_id, int type, const void *data, int len);
int mapif_guild_skillupack(int guild_id, uint16 skill_id, int account_id);
int mapif_guild_alliance(int guild_id1, int guild_id2, int account_id1, int account_id2, int flag, const char *name1, const char *name2);
int mapif_guild_position(struct guild *g, int idx);
int mapif_guild_notice(struct guild *g);
int mapif_guild_emblem(struct guild *g);
int mapif_guild_master_changed(struct guild *g, int aid, int cid);
int mapif_guild_castle_dataload(int fd, int sz, const int *castle_ids);
int mapif_parse_CreateGuild(int fd, int account_id, const char *name, const struct guild_member *master);
int mapif_parse_GuildInfo(int fd, int guild_id);
int mapif_parse_GuildAddMember(int fd, int guild_id, const struct guild_member *m);
int mapif_parse_GuildLeave(int fd, int guild_id, int account_id, int char_id, int flag, const char *mes);
int mapif_parse_GuildChangeMemberInfoShort(int fd, int guild_id, int account_id, int char_id, int online, int lv, int16 class);
int mapif_parse_BreakGuild(int fd, int guild_id);
int mapif_parse_GuildMessage(int fd, int guild_id, int account_id, const char *mes, int len);
int mapif_parse_GuildBasicInfoChange(int fd, int guild_id, int type, const void *data, int len);
int mapif_parse_GuildMemberInfoChange(int fd, int guild_id, int account_id, int char_id, int type, const char *data, int len);
int mapif_parse_GuildPosition(int fd, int guild_id, int idx, const struct guild_position *p);
int mapif_parse_GuildSkillUp(int fd, int guild_id, uint16 skill_id, int account_id, int max);
int mapif_parse_GuildDeleteAlliance(struct guild *g, int guild_id, int account_id1, int account_id2, int flag);
int mapif_parse_GuildAlliance(int fd, int guild_id1, int guild_id2, int account_id1, int account_id2, int flag);
int mapif_parse_GuildNotice(int fd, int guild_id, const char *mes1, const char *mes2);
int mapif_parse_GuildEmblem(int fd, int len, int guild_id, int dummy, const char *data);
int mapif_parse_GuildCastleDataLoad(int fd, int len, const int *castle_ids);
int mapif_parse_GuildCastleDataSave(int fd, int castle_id, int index, int value);
int mapif_parse_GuildMasterChange(int fd, int guild_id, const char* name, int len);
void mapif_homunculus_created(int fd, int account_id, const struct s_homunculus *sh, unsigned char flag);
void mapif_homunculus_deleted(int fd, int flag);
void mapif_homunculus_loaded(int fd, int account_id, struct s_homunculus *hd);
void mapif_homunculus_saved(int fd, int account_id, bool flag);
void mapif_homunculus_renamed(int fd, int account_id, int char_id, unsigned char flag, const char *name);
bool mapif_homunculus_create(struct s_homunculus *hd);
bool mapif_homunculus_save(const struct s_homunculus *hd);
bool mapif_homunculus_load(int homun_id, struct s_homunculus* hd);
bool mapif_homunculus_delete(int homun_id);
bool mapif_homunculus_rename(const char *name);
void mapif_parse_homunculus_create(int fd, int len, int account_id, const struct s_homunculus *phd);
void mapif_parse_homunculus_delete(int fd, int homun_id);
void mapif_parse_homunculus_load(int fd, int account_id, int homun_id);
void mapif_parse_homunculus_save(int fd, int len, int account_id, const struct s_homunculus *phd);
void mapif_parse_homunculus_rename(int fd, int account_id, int char_id, const char *name);
void mapif_mail_sendinbox(int fd, int char_id, unsigned char flag, struct mail_data *md);
void mapif_parse_mail_requestinbox(int fd);
void mapif_parse_mail_read(int fd);
void mapif_mail_sendattach(int fd, int char_id, struct mail_message *msg);
void mapif_mail_getattach(int fd, int char_id, int mail_id);
void mapif_parse_mail_getattach(int fd);
void mapif_mail_delete(int fd, int char_id, int mail_id, bool failed);
void mapif_parse_mail_delete(int fd);
void mapif_mail_new(struct mail_message *msg);
void mapif_mail_return(int fd, int char_id, int mail_id, int new_mail);
void mapif_parse_mail_return(int fd);
void mapif_mail_send(int fd, struct mail_message* msg);
void mapif_parse_mail_send(int fd);
bool mapif_mercenary_create(struct s_mercenary *merc);
bool mapif_mercenary_save(const struct s_mercenary *merc);
bool mapif_mercenary_load(int merc_id, int char_id, struct s_mercenary *merc);
bool mapif_mercenary_delete(int merc_id);
void mapif_mercenary_send(int fd, struct s_mercenary *merc, unsigned char flag);
void mapif_parse_mercenary_create(int fd, const struct s_mercenary *merc);
void mapif_parse_mercenary_load(int fd, int merc_id, int char_id);
void mapif_mercenary_deleted(int fd, unsigned char flag);
void mapif_parse_mercenary_delete(int fd, int merc_id);
void mapif_mercenary_saved(int fd, unsigned char flag);
void mapif_parse_mercenary_save(int fd, const struct s_mercenary *merc);
int mapif_party_created(int fd, int account_id, int char_id, struct party *p);
void mapif_party_noinfo(int fd, int party_id, int char_id);
void mapif_party_info(int fd, struct party* p, int char_id);
int mapif_party_memberadded(int fd, int party_id, int account_id, int char_id, int flag);
int mapif_party_optionchanged(int fd, struct party *p, int account_id, int flag);
int mapif_party_withdraw(int party_id,int account_id, int char_id);
int mapif_party_membermoved(struct party *p, int idx);
int mapif_party_broken(int party_id, int flag);
int mapif_party_message(int party_id, int account_id, const char *mes, int len, int sfd);
int mapif_parse_CreateParty(int fd, const char *name, int item, int item2, const struct party_member *leader);
void mapif_parse_PartyInfo(int fd, int party_id, int char_id);
int mapif_parse_PartyAddMember(int fd, int party_id, const struct party_member *member);
int mapif_parse_PartyChangeOption(int fd,int party_id,int account_id,int exp,int item);
int mapif_parse_PartyLeave(int fd, int party_id, int account_id, int char_id);
int mapif_parse_PartyChangeMap(int fd, int party_id, int account_id, int char_id, unsigned short map, int online, unsigned int lv);
int mapif_parse_BreakParty(int fd, int party_id);
int mapif_parse_PartyMessage(int fd, int party_id, int account_id, const char *mes, int len);
int mapif_parse_PartyLeaderChange(int fd, int party_id, int account_id, int char_id);
int mapif_pet_created(int fd, int account_id, struct s_pet *p);
int mapif_pet_info(int fd, int account_id, struct s_pet *p);
int mapif_pet_noinfo(int fd, int account_id);
int mapif_save_pet_ack(int fd, int account_id, int flag);
int mapif_delete_pet_ack(int fd, int flag);
int mapif_create_pet(int fd, int account_id, int char_id, short pet_class, short pet_lv, short pet_egg_id,
	short pet_equip, short intimate, short hungry, char rename_flag, char incubate, const char *pet_name);
int mapif_load_pet(int fd, int account_id, int char_id, int pet_id);
int mapif_save_pet(int fd, int account_id, const struct s_pet *data);
int mapif_delete_pet(int fd, int pet_id);
int mapif_parse_CreatePet(int fd);
int mapif_parse_LoadPet(int fd);
int mapif_parse_SavePet(int fd);
int mapif_parse_DeletePet(int fd);
struct quest *mapif_quests_fromsql(int char_id, int *count);
bool mapif_quest_delete(int char_id, int quest_id);
bool mapif_quest_add(int char_id, struct quest qd);
bool mapif_quest_update(int char_id, struct quest qd);
void mapif_quest_save_ack(int fd, int char_id, bool success);
int mapif_parse_quest_save(int fd);
void mapif_send_quests(int fd, int char_id, struct quest *tmp_questlog, int num_quests);
int mapif_parse_quest_load(int fd);
/* RoDEX */
int mapif_parse_rodex_requestinbox(int fd);
void mapif_rodex_sendinbox(int fd, int char_id, int8 opentype, int8 flag, int count, int64 mail_id, struct rodex_maillist *mails);
int mapif_parse_rodex_checkhasnew(int fd);
void mapif_rodex_sendhasnew(int fd, int char_id, bool has_new);
int mapif_parse_rodex_updatemail(int fd);
int mapif_parse_rodex_send(int fd);
void mapif_rodex_send(int fd, int sender_id, int receiver_id, int receiver_accountid, bool result);
int mapif_parse_rodex_checkname(int fd);
void mapif_rodex_checkname(int fd, int reqchar_id, int target_char_id, short target_class, int target_level, char *name);
int mapif_load_guild_storage(int fd,int account_id,int guild_id, char flag);
int mapif_save_guild_storage_ack(int fd, int account_id, int guild_id, int fail);
int mapif_parse_LoadGuildStorage(int fd);
int mapif_parse_SaveGuildStorage(int fd);
int mapif_account_storage_load(int fd, int account_id);
int mapif_parse_AccountStorageLoad(int fd);
int mapif_parse_AccountStorageSave(int fd);
void mapif_send_AccountStorageSaveAck(int fd, int account_id, bool save);
int mapif_itembound_ack(int fd, int aid, int guild_id);
int mapif_parse_ItemBoundRetrieve_sub(int fd);
void mapif_parse_ItemBoundRetrieve(int fd);
void mapif_parse_accinfo(int fd);
void mapif_parse_accinfo2(bool success, int map_fd, int u_fd, int u_aid, int account_id, const char *userid, const char *user_pass,
    const char *email, const char *last_ip, const char *lastlogin, const char *pin_code, const char *birthdate, int group_id, int logincount, int state);
int mapif_broadcast(const unsigned char *mes, int len, unsigned int fontColor, short fontType, short fontSize, short fontAlign, short fontY, int sfd);
int mapif_wis_message(struct WisData *wd);
void mapif_wis_response(int fd, const unsigned char *src, int flag);
int mapif_wis_end(struct WisData *wd, int flag);
int mapif_account_reg_reply(int fd,int account_id,int char_id, int type);
int mapif_disconnectplayer(int fd, int account_id, int char_id, int reason);
int mapif_parse_broadcast(int fd);
int mapif_parse_WisRequest(int fd);
int mapif_parse_WisReply(int fd);
int mapif_parse_WisToGM(int fd);
int mapif_parse_Registry(int fd);
int mapif_parse_RegistryRequest(int fd);
void mapif_namechange_ack(int fd, int account_id, int char_id, int type, int flag, const char *const name);
int mapif_parse_NameChangeRequest(int fd);

// Clan System
int mapif_parse_ClanMemberKick(int fd, int clan_id, int kick_interval)
{
	int count = 0;

	if (inter_clan->kick_inactive_members(clan_id, kick_interval) == 1)
		count = inter_clan->count_members(clan_id, kick_interval);

	WFIFOHEAD(fd, 10);
	WFIFOW(fd, 0) = 0x3858;
	WFIFOL(fd, 2) = clan_id;
	WFIFOL(fd, 6) = count;
	WFIFOSET(fd, 10);
	return 0;
}

int mapif_parse_ClanMemberCount(int fd, int clan_id, int kick_interval)
{
	WFIFOHEAD(fd, 10);
	WFIFOW(fd, 0) = 0x3858;
	WFIFOL(fd, 2) = clan_id;
	WFIFOL(fd, 6) = inter_clan->count_members(clan_id, kick_interval);
	WFIFOSET(fd, 10);
	return 0;
}

struct mapif_interface mapif_s;
struct mapif_interface *mapif;

void mapif_defaults(void) {
	mapif = &mapif_s;

	mapif->ban = mapif_ban;
	mapif->server_init = mapif_server_init;
	mapif->server_destroy = mapif_server_destroy;
	mapif->server_reset = mapif_server_reset;
	mapif->on_disconnect = mapif_on_disconnect;
	mapif->on_parse_accinfo = mapif_on_parse_accinfo;
	mapif->char_ban = mapif_char_ban;
	mapif->sendall = mapif_sendall;
	mapif->sendallwos = mapif_sendallwos;
	mapif->send = mapif_send;
	mapif->send_users_count = mapif_send_users_count;
	mapif->auction_message = mapif_auction_message;
	mapif->auction_sendlist = mapif_auction_sendlist;
	mapif->parse_auction_requestlist = mapif_parse_auction_requestlist;
	mapif->auction_register = mapif_auction_register;
	mapif->parse_auction_register = mapif_parse_auction_register;
	mapif->auction_cancel = mapif_auction_cancel;
	mapif->parse_auction_cancel = mapif_parse_auction_cancel;
	mapif->auction_close = mapif_auction_close;
	mapif->parse_auction_close = mapif_parse_auction_close;
	mapif->auction_bid = mapif_auction_bid;
	mapif->parse_auction_bid = mapif_parse_auction_bid;
	mapif->elemental_create = mapif_elemental_create;
	mapif->elemental_save = mapif_elemental_save;
	mapif->elemental_load = mapif_elemental_load;
	mapif->elemental_delete = mapif_elemental_delete;
	mapif->elemental_send = mapif_elemental_send;
	mapif->parse_elemental_create = mapif_parse_elemental_create;
	mapif->parse_elemental_load = mapif_parse_elemental_load;
	mapif->elemental_deleted = mapif_elemental_deleted;
	mapif->parse_elemental_delete = mapif_parse_elemental_delete;
	mapif->elemental_saved = mapif_elemental_saved;
	mapif->parse_elemental_save = mapif_parse_elemental_save;
	mapif->guild_created = mapif_guild_created;
	mapif->guild_noinfo = mapif_guild_noinfo;
	mapif->guild_info = mapif_guild_info;
	mapif->guild_memberadded = mapif_guild_memberadded;
	mapif->guild_withdraw = mapif_guild_withdraw;
	mapif->guild_memberinfoshort = mapif_guild_memberinfoshort;
	mapif->guild_broken = mapif_guild_broken;
	mapif->guild_message = mapif_guild_message;
	mapif->guild_basicinfochanged = mapif_guild_basicinfochanged;
	mapif->guild_memberinfochanged = mapif_guild_memberinfochanged;
	mapif->guild_skillupack = mapif_guild_skillupack;
	mapif->guild_alliance = mapif_guild_alliance;
	mapif->guild_position = mapif_guild_position;
	mapif->guild_notice = mapif_guild_notice;
	mapif->guild_emblem = mapif_guild_emblem;
	mapif->guild_master_changed = mapif_guild_master_changed;
	mapif->guild_castle_dataload = mapif_guild_castle_dataload;
	mapif->parse_CreateGuild = mapif_parse_CreateGuild;
	mapif->parse_GuildInfo = mapif_parse_GuildInfo;
	mapif->parse_GuildAddMember = mapif_parse_GuildAddMember;
	mapif->parse_GuildLeave = mapif_parse_GuildLeave;
	mapif->parse_GuildChangeMemberInfoShort = mapif_parse_GuildChangeMemberInfoShort;
	mapif->parse_BreakGuild = mapif_parse_BreakGuild;
	mapif->parse_GuildMessage = mapif_parse_GuildMessage;
	mapif->parse_GuildBasicInfoChange = mapif_parse_GuildBasicInfoChange;
	mapif->parse_GuildMemberInfoChange = mapif_parse_GuildMemberInfoChange;
	mapif->parse_GuildPosition = mapif_parse_GuildPosition;
	mapif->parse_GuildSkillUp = mapif_parse_GuildSkillUp;
	mapif->parse_GuildDeleteAlliance = mapif_parse_GuildDeleteAlliance;
	mapif->parse_GuildAlliance = mapif_parse_GuildAlliance;
	mapif->parse_GuildNotice = mapif_parse_GuildNotice;
	mapif->parse_GuildEmblem = mapif_parse_GuildEmblem;
	mapif->parse_GuildCastleDataLoad = mapif_parse_GuildCastleDataLoad;
	mapif->parse_GuildCastleDataSave = mapif_parse_GuildCastleDataSave;
	mapif->parse_GuildMasterChange = mapif_parse_GuildMasterChange;
	mapif->homunculus_created = mapif_homunculus_created;
	mapif->homunculus_deleted = mapif_homunculus_deleted;
	mapif->homunculus_loaded = mapif_homunculus_loaded;
	mapif->homunculus_saved = mapif_homunculus_saved;
	mapif->homunculus_renamed = mapif_homunculus_renamed;
	mapif->homunculus_create = mapif_homunculus_create;
	mapif->homunculus_save = mapif_homunculus_save;
	mapif->homunculus_load = mapif_homunculus_load;
	mapif->homunculus_delete = mapif_homunculus_delete;
	mapif->homunculus_rename = mapif_homunculus_rename;
	mapif->parse_homunculus_create = mapif_parse_homunculus_create;
	mapif->parse_homunculus_delete = mapif_parse_homunculus_delete;
	mapif->parse_homunculus_load = mapif_parse_homunculus_load;
	mapif->parse_homunculus_save = mapif_parse_homunculus_save;
	mapif->parse_homunculus_rename = mapif_parse_homunculus_rename;
	mapif->mail_sendinbox = mapif_mail_sendinbox;
	mapif->parse_mail_requestinbox = mapif_parse_mail_requestinbox;
	mapif->parse_mail_read = mapif_parse_mail_read;
	mapif->mail_sendattach = mapif_mail_sendattach;
	mapif->mail_getattach = mapif_mail_getattach;
	mapif->parse_mail_getattach = mapif_parse_mail_getattach;
	mapif->mail_delete = mapif_mail_delete;
	mapif->parse_mail_delete = mapif_parse_mail_delete;
	mapif->mail_new = mapif_mail_new;
	mapif->mail_return = mapif_mail_return;
	mapif->parse_mail_return = mapif_parse_mail_return;
	mapif->mail_send = mapif_mail_send;
	mapif->parse_mail_send = mapif_parse_mail_send;
	mapif->mercenary_create = mapif_mercenary_create;
	mapif->mercenary_save = mapif_mercenary_save;
	mapif->mercenary_load = mapif_mercenary_load;
	mapif->mercenary_delete = mapif_mercenary_delete;
	mapif->mercenary_send = mapif_mercenary_send;
	mapif->parse_mercenary_create = mapif_parse_mercenary_create;
	mapif->parse_mercenary_load = mapif_parse_mercenary_load;
	mapif->mercenary_deleted = mapif_mercenary_deleted;
	mapif->parse_mercenary_delete = mapif_parse_mercenary_delete;
	mapif->mercenary_saved = mapif_mercenary_saved;
	mapif->parse_mercenary_save = mapif_parse_mercenary_save;
	mapif->party_created = mapif_party_created;
	mapif->party_noinfo = mapif_party_noinfo;
	mapif->party_info = mapif_party_info;
	mapif->party_memberadded = mapif_party_memberadded;
	mapif->party_optionchanged = mapif_party_optionchanged;
	mapif->party_withdraw = mapif_party_withdraw;
	mapif->party_membermoved = mapif_party_membermoved;
	mapif->party_broken = mapif_party_broken;
	mapif->party_message = mapif_party_message;
	mapif->parse_CreateParty = mapif_parse_CreateParty;
	mapif->parse_PartyInfo = mapif_parse_PartyInfo;
	mapif->parse_PartyAddMember = mapif_parse_PartyAddMember;
	mapif->parse_PartyChangeOption = mapif_parse_PartyChangeOption;
	mapif->parse_PartyLeave = mapif_parse_PartyLeave;
	mapif->parse_PartyChangeMap = mapif_parse_PartyChangeMap;
	mapif->parse_BreakParty = mapif_parse_BreakParty;
	mapif->parse_PartyMessage = mapif_parse_PartyMessage;
	mapif->parse_PartyLeaderChange = mapif_parse_PartyLeaderChange;
	mapif->pet_created = mapif_pet_created;
	mapif->pet_info = mapif_pet_info;
	mapif->pet_noinfo = mapif_pet_noinfo;
	mapif->save_pet_ack = mapif_save_pet_ack;
	mapif->delete_pet_ack = mapif_delete_pet_ack;
	mapif->create_pet = mapif_create_pet;
	mapif->load_pet = mapif_load_pet;
	mapif->save_pet = mapif_save_pet;
	mapif->delete_pet = mapif_delete_pet;
	mapif->parse_CreatePet = mapif_parse_CreatePet;
	mapif->parse_LoadPet = mapif_parse_LoadPet;
	mapif->parse_SavePet = mapif_parse_SavePet;
	mapif->parse_DeletePet = mapif_parse_DeletePet;
	mapif->quests_fromsql = mapif_quests_fromsql;
	mapif->quest_delete = mapif_quest_delete;
	mapif->quest_add = mapif_quest_add;
	mapif->quest_update = mapif_quest_update;
	mapif->quest_save_ack = mapif_quest_save_ack;
	mapif->parse_quest_save = mapif_parse_quest_save;
	mapif->send_quests = mapif_send_quests;
	mapif->parse_quest_load = mapif_parse_quest_load;
	/* RoDEX */
	mapif->parse_rodex_requestinbox = mapif_parse_rodex_requestinbox;
	mapif->rodex_sendinbox = mapif_rodex_sendinbox;
	mapif->parse_rodex_checkhasnew = mapif_parse_rodex_checkhasnew;
	mapif->rodex_sendhasnew = mapif_rodex_sendhasnew;
	mapif->parse_rodex_updatemail = mapif_parse_rodex_updatemail;
	mapif->parse_rodex_send = mapif_parse_rodex_send;
	mapif->rodex_send = mapif_rodex_send;
	mapif->parse_rodex_checkname = mapif_parse_rodex_checkname;
	mapif->rodex_checkname = mapif_rodex_checkname;
	mapif->load_guild_storage = mapif_load_guild_storage;
	mapif->save_guild_storage_ack = mapif_save_guild_storage_ack;
	mapif->parse_LoadGuildStorage = mapif_parse_LoadGuildStorage;
	mapif->parse_SaveGuildStorage = mapif_parse_SaveGuildStorage;
	mapif->pAccountStorageLoad = mapif_parse_AccountStorageLoad;
	mapif->pAccountStorageSave = mapif_parse_AccountStorageSave;
	mapif->sAccountStorageSaveAck = mapif_send_AccountStorageSaveAck;
	mapif->account_storage_load = mapif_account_storage_load;
	mapif->itembound_ack = mapif_itembound_ack;
	mapif->parse_ItemBoundRetrieve_sub = mapif_parse_ItemBoundRetrieve_sub;
	mapif->parse_ItemBoundRetrieve = mapif_parse_ItemBoundRetrieve;
	mapif->parse_accinfo = mapif_parse_accinfo;
	mapif->parse_accinfo2 = mapif_parse_accinfo2;
	mapif->broadcast = mapif_broadcast;
	mapif->wis_message = mapif_wis_message;
	mapif->wis_response = mapif_wis_response;
	mapif->wis_end = mapif_wis_end;
	mapif->account_reg_reply = mapif_account_reg_reply;
	mapif->disconnectplayer = mapif_disconnectplayer;
	mapif->parse_broadcast = mapif_parse_broadcast;
	mapif->parse_WisRequest = mapif_parse_WisRequest;
	mapif->parse_WisReply = mapif_parse_WisReply;
	mapif->parse_WisToGM = mapif_parse_WisToGM;
	mapif->parse_Registry = mapif_parse_Registry;
	mapif->parse_RegistryRequest = mapif_parse_RegistryRequest;
	mapif->namechange_ack = mapif_namechange_ack;
	mapif->parse_NameChangeRequest = mapif_parse_NameChangeRequest;
	/* Clan System */
	mapif->parse_ClanMemberKick = mapif_parse_ClanMemberKick;
	mapif->parse_ClanMemberCount = mapif_parse_ClanMemberCount;
}
