
print(typeof clearTimeout);
const id = setTimeout(
    () => {
        print("Hello");
    },
    3000
);

print(id);

clearTimeout(id);