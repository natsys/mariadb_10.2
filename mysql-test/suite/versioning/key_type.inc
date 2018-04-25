--disable_query_log
if ($MTR_COMBINATION_UNIQUE)
{
    set @KEY_TYPE= 'unique';
}
if ($MTR_COMBINATION_PK)
{
    set @KEY_TYPE= 'primary key';
}

delimiter ~~;
create procedure create_table_key(name varchar(255), cols varchar(255))
begin
  set @str= concat('call create_table("', name, '", "id int ', @KEY_TYPE, ', ', cols, '")');
  prepare ct_key_stmt from @str; execute ct_key_stmt; drop prepare ct_key_stmt;
end~~
delimiter ;~~
--enable_query_log
