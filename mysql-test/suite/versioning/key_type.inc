--disable_query_log
if ($MTR_COMBINATION_UNIQUE)
{
    let $KEY_TYPE= unique;
}
if ($MTR_COMBINATION_PRIMARY)
{
    let $KEY_TYPE= primary key;
}
if ($MTR_COMBINATION_SECONDARY)
{
    let $KEY_TYPE= key;
}
--enable_query_log
