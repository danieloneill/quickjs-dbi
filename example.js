import * as DBI from 'dbi.so';
import * as std from 'std';

function dbitest() {
	const db = DBI.open("sqlite3", { "dbname":"test.sqlite3", "sqlite3_dbdir":"." });
	
	// create if needed
	db.exec(`
	CREATE TABLE IF NOT EXISTS test(
	    foo TEXT,
	    bar INTEGER,
	    whizz DECIMAL(6,3),
	    bang BOOLEAN,
	    woop DATETIME
	);
	`);
	
	// insert some sample data if empty
	let rows = db.query("SELECT COUNT(*) AS n FROM test").toArray(true);
	if (rows[0].n === 0) {
	    db.exec(`INSERT INTO test (foo, bar, whizz, bang, woop)VALUES
	        ('hello', 42, 3.141, 1, datetime('now','-1 day','localtime')),
	        ('world', -7, 2.718, 0, datetime('2024-04-12 12:30:45.789')),
	        ('quickjs', 1337, 1.618, 1, datetime('now'))
	    ;`);
	}
	
	// doing it manually:
	console.log("=== manual output, array bind ===");
	let res = db.query("SELECT * FROM test WHERE bar > ?", [5]);
	let numrows = res.numrows();
	let numfields = res.numfields();
	console.log(`Got ${numrows} rows, and ${numfields} fields.`);
	while( res.next() )
	{
		let row = [];
		for( let a=0; a < numfields; a++ )
			row.push( res.get(a) );
		console.log(JSON.stringify(row, null, 2));
	}
	
	console.log("=== array output, dict bind ===");
	res = db.query("SELECT foo, bar, whizz, bang, woop FROM test WHERE whizz >= :whizz", {'whizz':2.0});
	let arrRows = res.toArray();
	for (let row of arrRows)
	    console.log(JSON.stringify(row));
	
	console.log("=== dict output, naked bind ===");
	res = db.query("SELECT * FROM test");
	let dictRows = res.toArray(true);
	for (let row of dictRows)
	    console.log(JSON.stringify(row));
	
	db.close();
}

dbitest();

std.gc();

